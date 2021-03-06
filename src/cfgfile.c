/*
 * Copyright 2014 LKC Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "cfgfile.h"
#include "mbr.h"
#include "functions.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>

// Global variable for passing the top level cfg pointer through libconfuse
// This is needed for validating some of the function calls.
static cfg_t *toplevel_cfg;

/* function callback
 */
static int cb_func(enum fun_context_type ctype, cfg_t *cfg, cfg_opt_t *opt, int argc, const char **argv)
{
    struct fun_context fctx;
    memset(&fctx, 0, sizeof(fctx));
    fctx.type = ctype;
    fctx.cfg = toplevel_cfg;
    fctx.task = cfg;

    // Convert to the normal argc/argv
    fctx.argc = argc + 1;
    if (fctx.argc > FUN_MAX_ARGS || fctx.argc < 1) {
        cfg_error(cfg, "Too many arguments passed to '%s'", opt->name);
        return -1;
    }

    fctx.argv[0] = opt->name;
    memcpy(&fctx.argv[1], argv, sizeof(const char *) * argc);

    if (fun_validate(&fctx) < 0) {
        cfg_error(cfg, last_error());
        return -1;
    }

    char str_argc[5];
    sprintf(str_argc, "%d", fctx.argc);

    cfg_addlist(cfg, "funlist", 2, str_argc, fctx.argv[0]);
    int i;
    for (i = 1; i < fctx.argc; i++)
        cfg_addlist(cfg, "funlist", 1, fctx.argv[i]);

    return 0;
}

static int cb_on_init_func(cfg_t *cfg, cfg_opt_t *opt, int argc, const char **argv)
{
    return cb_func(FUN_CONTEXT_INIT, cfg, opt, argc, argv);
}

static int cb_on_finish_func(cfg_t *cfg, cfg_opt_t *opt, int argc, const char **argv)
{
    return cb_func(FUN_CONTEXT_FINISH, cfg, opt, argc, argv);
}

static int cb_on_error_func(cfg_t *cfg, cfg_opt_t *opt, int argc, const char **argv)
{
    return cb_func(FUN_CONTEXT_ERROR, cfg, opt, argc, argv);
}

static int cb_on_resource_func(cfg_t *cfg, cfg_opt_t *opt, int argc, const char **argv)
{
    return cb_func(FUN_CONTEXT_FILE, cfg, opt, argc, argv);
}

static int cb_define_bang(cfg_t *cfg, cfg_opt_t *opt, int argc, const char **argv)
{
    if(argc != 2) {
        cfg_error(cfg, "Too few parameters for the '%s' function",
                  opt->name);
        return -1;
    }

    INFO("Defining '%s'='%s'\n", argv[0], argv[1]);

    // Overwrite the environment.
    if (setenv(argv[0], argv[1], 1) < 0) {
        cfg_error(cfg, "setenv failed");
        return -1;
    }

    return 0;
}

static int cb_define(cfg_t *cfg, cfg_opt_t *opt, int argc, const char **argv)
{
    // Non-overwriting version of define
    if(argc != 2) {
        cfg_error(cfg, "Too few parameters for the '%s' function",
                  opt->name);
        return -1;
    }

    if (!getenv(argv[0])) {
        INFO("Defining '%s'='%s'\n", argv[0], argv[1]);

        if (setenv(argv[0], argv[1], 0) < 0) {
            cfg_error(cfg, "setenv failed");
            return -1;
        }
    } else {
        INFO("Not defining '%s'. Already set to '%s'\n", argv[0], getenv(argv[0]));
    }
    return 0;
}

static int cb_validate_file_resource(cfg_t *cfg, cfg_opt_t *opt)
{
    // This is called for each file-resource, so we only need to
    // validate the last one.
    cfg_t *sec = cfg_opt_getnsec(opt, cfg_opt_size(opt) - 1);
    if(!sec)
    {
        cfg_error(cfg, "section is NULL!?");
        return -1;
    }
    const char *path = cfg_getstr(sec, "host-path");
    if (!path) {
        cfg_error(cfg, "host-path must be set for file-report '%s'", cfg_title(sec));
        return -1;
    }

    return 0;
}

static int cb_validate_mbr(cfg_t *cfg, cfg_opt_t *opt)
{
    // This is called for each mbr, so we only need to
    // validate the last one.
    cfg_t *sec = cfg_opt_getnsec(opt, cfg_opt_size(opt) - 1);
    if(!sec)
    {
        cfg_error(cfg, "section is NULL!?");
        return -1;
    }

    const char *path = cfg_getstr(sec, "bootstrap-code-host-path");
    const char *bootstrap_hex = cfg_getstr(sec, "bootstrap-code");
    if (path && !bootstrap_hex) {
        FILE *fp = fopen(path, "r");
        if (!fp) {
            cfg_error(cfg, "mbr bootstrap code path '%s' required, but can't be read", path);
            return -1;
        }

        uint8_t bootstrap[440];
        if (fread(bootstrap, 1, sizeof(bootstrap), fp) != sizeof(bootstrap)) {
            cfg_error(cfg, "mbr bootstrap code in '%s' should be %d bytes", path, sizeof(bootstrap));
            fclose(fp);
            return -1;
        }
        fclose(fp);

        char bootstrap_str[sizeof(bootstrap) * 2 + 1];
        bytes_to_hex(bootstrap, bootstrap_str, sizeof(bootstrap));
        cfg_setstr(sec, "bootstrap-code", bootstrap_str);
    }
    if (mbr_verify_cfg(sec) < 0)
        cfg_error(cfg, last_error());

    return 0;
}

static cfg_opt_t file_resource_opts[] = {
    CFG_STR("host-path", 0, CFGF_NONE),
    CFG_INT("length", 0, CFGF_NONE),
    CFG_STR("blake2b-256", 0, CFGF_NONE),
    CFG_STR("sha256", 0, CFGF_NONE), // Old hash for files - use blake2b-256 now
    CFG_END()
};
static cfg_opt_t mbr_partition_opts[] = {
    CFG_INT("block-offset", -1, CFGF_NONE),
    CFG_INT("block-count", -1, CFGF_NONE),
    CFG_INT("type", -1, CFGF_NONE),
    CFG_BOOL("boot", cfg_false, CFGF_NONE),
    CFG_END()
};
static cfg_opt_t mbr_osii_opts[] = {
    CFG_INT("os-major", 0, CFGF_NONE),
    CFG_INT("os-minor", 0, CFGF_NONE),
    CFG_INT("start-block-offset", 0, CFGF_NONE),
    CFG_INT("ddr-load-address", 0, CFGF_NONE),
    CFG_INT("entry-point", 0, CFGF_NONE),
    CFG_INT("image-size-blocks", 0, CFGF_NONE),
    CFG_INT("attribute", 0xf, CFGF_NONE),
    CFG_END()
};
static cfg_opt_t mbr_opts[] = {
    CFG_STR("bootstrap-code-host-path", 0, CFGF_NONE),
    CFG_STR("bootstrap-code", 0, CFGF_NONE),
    CFG_BOOL("include-osip", cfg_false, CFGF_NONE),
    CFG_INT("osip-major", 1, CFGF_NONE),
    CFG_INT("osip-minor", 0, CFGF_NONE),
    CFG_INT("osip-num-pointers", 1, CFGF_NONE),
    CFG_SEC("partition", mbr_partition_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
    CFG_SEC("osii", mbr_osii_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
    CFG_END()
};

#define CFG_ON_EVENT_FUNCTIONS(CB) \
    CFG_STR_LIST("funlist", 0, CFGF_NONE), \
    CFG_FUNC("raw_write", CB), \
    CFG_FUNC("fat_mkfs", CB), \
    CFG_FUNC("fat_attrib", CB), \
    CFG_FUNC("fat_write", CB), \
    CFG_FUNC("fat_cp", CB), \
    CFG_FUNC("fat_mv", CB), \
    CFG_FUNC("fat_rm", CB), \
    CFG_FUNC("fat_mkdir", CB), \
    CFG_FUNC("fat_setlabel", CB), \
    CFG_FUNC("fw_create", CB), \
    CFG_FUNC("fw_add_local_file", CB), \
    CFG_FUNC("mbr_write", CB)

static cfg_opt_t task_on_init_opts[] = {
    CFG_ON_EVENT_FUNCTIONS(cb_on_init_func),
    CFG_END()
};
static cfg_opt_t task_on_finish_opts[] = {
    CFG_ON_EVENT_FUNCTIONS(cb_on_finish_func),
    CFG_END()
};
static cfg_opt_t task_on_error_opts[] = {
    CFG_ON_EVENT_FUNCTIONS(cb_on_error_func),
    CFG_END()
};
static cfg_opt_t task_on_resource_opts[] = {
    CFG_STR("verify-on-the-fly", cfg_false, CFGF_NONE),
    CFG_ON_EVENT_FUNCTIONS(cb_on_resource_func),
    CFG_END()
};
static cfg_opt_t task_opts[] = {
    CFG_INT("require-partition1-offset", -1, CFGF_NONE),
    CFG_BOOL("verify-on-the-fly", cfg_false, CFGF_NONE),
    CFG_BOOL("require-unmounted-destination", cfg_false, CFGF_NONE),
    CFG_SEC("on-init", task_on_init_opts, CFGF_NONE),
    CFG_SEC("on-finish", task_on_finish_opts, CFGF_NONE),
    CFG_SEC("on-error", task_on_error_opts, CFGF_NONE),
    CFG_SEC("on-resource", task_on_resource_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
    CFG_END()
};
cfg_opt_t opts[] = {
    CFG_STR("meta-product", 0, CFGF_NONE),
    CFG_STR("meta-description", 0, CFGF_NONE),
    CFG_STR("meta-version", 0, CFGF_NONE),
    CFG_STR("meta-author", 0, CFGF_NONE),
    CFG_STR("meta-platform", 0, CFGF_NONE),
    CFG_STR("meta-architecture", 0, CFGF_NONE),
    CFG_STR("meta-creation-date", 0, CFGF_NONE),

    CFG_STR("require-fwup-version", "0.0", CFGF_NONE),
    CFG_FUNC("define", cb_define),
    CFG_FUNC("define!", cb_define_bang),
    CFG_SEC("file-resource", file_resource_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
    CFG_SEC("mbr", mbr_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
    CFG_SEC("task", task_opts, CFGF_MULTI | CFGF_TITLE | CFGF_NO_TITLE_DUPES),
    CFG_FUNC("include", &cfg_include),
    CFG_END()
};

int cfgfile_parse_file(const char *filename, cfg_t **cfg)
{
    if (fwup_verbose) {
        extern char **environ;
        int e = 0;

        INFO("Config environment:\n");
        while (environ[e] != NULL) {
            INFO(" %s\n", environ[e]);
            e++;
        }
    }

    int rc = 0;
    toplevel_cfg = cfg_init(opts, 0);

    // set a validating callback function for sections
    cfg_set_validate_func(toplevel_cfg, "file-resource", cb_validate_file_resource);
    cfg_set_validate_func(toplevel_cfg, "mbr", cb_validate_mbr);
    switch (cfg_parse(toplevel_cfg, filename)) {
    case CFG_SUCCESS:
        break;
    case CFG_FILE_ERROR:
        ERR_CLEANUP_MSG("Error opening configuration file '%s'", filename);
    default:
    case CFG_PARSE_ERROR:
        ERR_CLEANUP_MSG("Error parsing configuration file '%s'", filename);
    }
    *cfg = toplevel_cfg;
    return 0;

cleanup:
    cfg_free(toplevel_cfg);
    return rc;
}

/**
 * Helper function for reading all data in a file in the archive
 *
 * @param a the archive
 * @param buffer where to put the data
 * @param total_size how much to read
 * @return 0 on success
 */
int archive_read_all_data(struct archive *a, char *buffer, ssize_t total_size)
{
    ssize_t size_left = total_size;
    while (size_left > 0) {
        ssize_t len = archive_read_data(a, &buffer[total_size - size_left], size_left);
        if (len <= 0)
            return -1;
        size_left -= len;
    }
    return 0;
}

int cfgfile_parse_fw_ae(struct archive *a,
                        struct archive_entry *ae,
                        cfg_t **cfg,
                        unsigned char *meta_conf_signature,
                        const unsigned char *public_key)
{
    int rc = 0;
    char *meta_conf = NULL;

    if (!archive_entry_size_is_set(ae))
        ERR_CLEANUP_MSG("Expecting meta.conf size to be set");

    ssize_t total_size = archive_entry_size(ae);
    if (total_size < 10 || total_size > 50000)
        ERR_CLEANUP_MSG("Unexpected meta.conf size: %d", total_size);

    meta_conf = (char *) malloc(total_size + 1);
    if (archive_read_all_data(a, meta_conf, total_size) < 0)
        ERR_CLEANUP_MSG("Error reading meta.conf from archive.\n"
                        "Check for file corruption or libarchive built without zlib support");
    meta_conf[total_size] = 0;

    // Check the signature on meta.conf if it has been signed
    if (public_key) {
        if (!meta_conf_signature)
            ERR_CLEANUP_MSG("Expecting signed firmware archive.");

        if (crypto_sign_verify_detached(meta_conf_signature, (unsigned char *) meta_conf, total_size, public_key) != 0)
            ERR_CLEANUP_MSG("Firmware archive's meta.conf fails digital signature verification.");
    } else if (meta_conf_signature) {
        INFO("Firmware archive is signed, but signature verification is off.");
    }

    // Parse the configuration, but do minimal validity checking of configuration
    // since many things are only used on the creation of the firmware update.
    *cfg = cfg_init(opts, 0);
    if (cfg_parse_buf(*cfg, meta_conf) != 0)
        ERR_CLEANUP_MSG("Unexpected error parsing meta.conf");

cleanup:
    if (meta_conf)
        free(meta_conf);

    return rc;
}

int cfgfile_parse_fw_meta_conf(const char *filename, cfg_t **cfg, const unsigned char *public_key)
{
    int rc = 0;
    unsigned char *meta_conf_signature = NULL;
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    rc = archive_read_open_filename(a, filename, 16384);
    if (rc != ARCHIVE_OK)
        ERR_CLEANUP_MSG("Cannot open archive '%s'", filename);

    struct archive_entry *ae;
    rc = archive_read_next_header(a, &ae);
    if (rc != ARCHIVE_OK)
        ERR_CLEANUP_MSG("Error reading archive '%s'", filename);

    if (strcmp(archive_entry_pathname(ae), "meta.conf.ed25519") == 0) {
        ssize_t total_size = archive_entry_size(ae);
        if (total_size != crypto_sign_BYTES)
            ERR_CLEANUP_MSG("Unexpected meta.conf.ed25519 size: %d", total_size);

        meta_conf_signature = (unsigned char *) malloc(total_size);
        if (archive_read_all_data(a, (char *) meta_conf_signature, total_size) < 0)
            ERR_CLEANUP_MSG("Error reading meta.conf.ed25519 from archive.\n"
                            "Check for file corruption or libarchive built without zlib support");

        rc = archive_read_next_header(a, &ae);
        if (rc != ARCHIVE_OK)
            ERR_CLEANUP_MSG("Expecting more than meta.conf.ed25519 in archive");
    }
    if (strcmp(archive_entry_pathname(ae), "meta.conf") != 0)
        ERR_CLEANUP_MSG("Expecting meta.conf to be at beginning of %s", filename);

    OK_OR_CLEANUP(cfgfile_parse_fw_ae(a, ae, cfg, meta_conf_signature, public_key));

cleanup:
    archive_read_free(a);
    if (meta_conf_signature)
        free(meta_conf_signature);

    return rc;
}

void cfgfile_free(cfg_t *cfg)
{
    cfg_free(cfg);
}
