/* createrepo_c - Library of routines for manipulation with repodata
 * Copyright (C) 2012  Tomas Mlcoch
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include "version.h"
#include "constants.h"
#include "parsepkg.h"
#include <fcntl.h>
#include "locate_metadata.h"
#include "load_metadata.h"
#include "repomd.h"
#include "compression_wrapper.h"
#include "misc.h"
#include "cmd_parser.h"
#include "xml_dump.h"
#include "sqlite.h"


#define G_LOG_DOMAIN    ((gchar*) 0)



struct UserData {
    CW_FILE *pri_f;
    CW_FILE *fil_f;
    CW_FILE *oth_f;
    DbPrimaryStatements pri_statements;
    DbFilelistsStatements fil_statements;
    DbOtherStatements oth_statements;
    int changelog_limit;
    const char *location_base;
    int repodir_name_len;
    const char *checksum_type_str;
    ChecksumType checksum_type;
    gboolean quiet;
    gboolean verbose;
    gboolean skip_symlinks;
    int package_count;

    // Update stuff
    gboolean skip_stat;
    GHashTable *old_metadata;
};


struct PoolTask {
    char* full_path;
    char* filename;
    char* path;
};


// Global variables used by signal handler
char *tmp_repodata_path = NULL;


// Signal handler
void sigint_catcher(int sig)
{
    UNUSED(sig);
    g_message("SIGINT catched: Terminating...");
    if (tmp_repodata_path) {
        remove_dir(tmp_repodata_path);
    }
    exit(1);
}



int allowed_file(const gchar *filename, struct CmdOptions *options)
{
    // Check file against exclude glob masks
    if (options->exclude_masks) {
        int str_len = strlen(filename);
        gchar *reversed_filename = g_utf8_strreverse(filename, str_len);

        GSList *element;
        for (element=options->exclude_masks; element; element=g_slist_next(element)) {
            if (g_pattern_match((GPatternSpec *) element->data, str_len, filename, reversed_filename)) {
                g_free(reversed_filename);
                g_debug("Exclude masks hit - skipping: %s", filename);
                return FALSE;
            }
        }
        g_free(reversed_filename);
    }
    return TRUE;
}



#define LOCK_PRI        0
#define LOCK_FIL        1
#define LOCK_OTH        2

G_LOCK_DEFINE (LOCK_PRI);
G_LOCK_DEFINE (LOCK_FIL);
G_LOCK_DEFINE (LOCK_OTH);


void dumper_thread(gpointer data, gpointer user_data) {

    struct UserData *udata = (struct UserData *) user_data;
    struct PoolTask *task = (struct PoolTask *) data;

    // get location_href without leading part of path (path to repo) including '/' char
    const char *location_href = task->full_path + udata->repodir_name_len;

    const char *location_base = udata->location_base;

    // Get stat info about file
    struct stat stat_buf;
    if (udata->old_metadata && !(udata->skip_stat)) {
        if (stat(task->full_path, &stat_buf) == -1) {
            g_critical("Stat() on %s: %s", task->full_path, strerror(errno));
            return;
        }
    }

    struct XmlStruct res;

    // Update stuff
    gboolean old_used = FALSE;
    Package *md = NULL;
    Package *pkg = NULL;

    if (udata->old_metadata) {
        // We have old metadata
        md = (Package *) g_hash_table_lookup (udata->old_metadata, task->filename);
        if (md) {
            // CACHE HIT!

            g_debug("CACHE HIT %s", task->filename);

            if (udata->skip_stat) {
                old_used = TRUE;
            } else if (stat_buf.st_mtime == md->time_file
                       && stat_buf.st_size == md->size_package
                       && !strcmp(udata->checksum_type_str, md->checksum_type))
            {
                old_used = TRUE;
            } else {
                g_debug("%s metadata are obsolete -> generating new", task->filename);
            }
        }

        if (old_used) {
            // We have usable old data, but we have to set locations (href and base)
            md->location_href = (char *) location_href;
            md->location_base = (char *) location_base;
        }
    }

    if (!old_used) {
        pkg = package_from_file(task->full_path, udata->checksum_type,
                                location_href, udata->location_base,
                                udata->changelog_limit, NULL);
        if (!pkg) {
            g_warning("Cannot read package: %s", task->full_path);
            goto task_cleanup;
        }
        res = xml_dump(pkg);
    } else {
        pkg = md;
        res = xml_dump(md);
    }

    // Write primary data
    G_LOCK(LOCK_PRI);
    cw_puts(udata->pri_f, (const char *) res.primary);
    if (udata->pri_statements) {
        add_primary_pkg_db(udata->pri_statements, pkg);
    }
    G_UNLOCK(LOCK_PRI);

    // Write fielists data
    G_LOCK(LOCK_FIL);
    cw_puts(udata->fil_f, (const char *) res.filelists);
    if (udata->fil_statements) {
        add_filelists_pkg_db(udata->fil_statements, pkg);
    }
    G_UNLOCK(LOCK_FIL);

    // Write other data
    G_LOCK(LOCK_OTH);
    cw_puts(udata->oth_f, (const char *) res.other);
    if (udata->oth_statements) {
        add_other_pkg_db(udata->oth_statements, pkg);
    }
    G_UNLOCK(LOCK_OTH);


    // Clean up

    if (pkg != md) {
        package_free(pkg);
    }

    free(res.primary);
    free(res.filelists);
    free(res.other);

task_cleanup:
    g_free(task->full_path);
    g_free(task->filename);
    g_free(task->path);
    g_free(task);

    return;
}



int main(int argc, char **argv) {


    // Arguments parsing

    struct CmdOptions *cmd_options;
    cmd_options = parse_arguments(&argc, &argv);
    if (!cmd_options) {
        exit(1);
    }


    // Arguments pre-check

    if (cmd_options->version) {
        printf("Version: %d.%d.%d\n", MAJOR_VERSION, MINOR_VERSION, PATCH_VERSION);
        free_options(cmd_options);
        exit(0);
    } else if (argc != 2) {
        fprintf(stderr, "Must specify exactly one directory to index.\n");
        fprintf(stderr, "Usage: %s [options] <directory_to_index>\n\n", get_filename(argv[0]));
        free_options(cmd_options);
        exit(1);
    }


    // Dirs

    gchar *in_dir   = NULL;     // path/to/repo/
    gchar *in_repo  = NULL;     // path/to/repo/repodata/
    gchar *out_dir  = NULL;     // path/to/out_repo/
    gchar *out_repo = NULL;     // path/to/out_repo/repodata/
    gchar *tmp_out_repo = NULL; // path/to/out_repo/.repodata/

    in_dir = normalize_dir_path(argv[1]);
    cmd_options->input_dir = g_strdup(in_dir);


    // Check if inputdir exists

    if (!g_file_test(cmd_options->input_dir, G_FILE_TEST_EXISTS|G_FILE_TEST_IS_DIR)) {
        g_warning("Directory %s must exist", cmd_options->input_dir);
        g_free(in_dir);
        free_options(cmd_options);
        exit(1);
    }


    // Check parsed arguments

    if (!check_arguments(cmd_options)) {
        g_free(in_dir);
        free_options(cmd_options);
        exit(1);
    }


    // Set logging stuff

    g_log_set_default_handler (log_function, NULL);

    if (cmd_options->quiet) {
        // Quiet mode
        GLogLevelFlags levels = G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_WARNING;
        g_log_set_handler(NULL, levels, black_hole_log_function, NULL);
        g_log_set_handler("C_CREATEREPOLIB", levels, black_hole_log_function, NULL);
    } else if (cmd_options->verbose) {
        // Verbose mode
        GLogLevelFlags levels = G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO | G_LOG_LEVEL_DEBUG | G_LOG_LEVEL_WARNING;
        g_log_set_handler(NULL, levels, log_function, NULL);
        g_log_set_handler("C_CREATEREPOLIB", levels, log_function, NULL);
    } else {
        // Standard mode
        GLogLevelFlags levels = G_LOG_LEVEL_DEBUG;
        g_log_set_handler(NULL, levels, black_hole_log_function, NULL);
        g_log_set_handler("C_CREATEREPOLIB", levels, black_hole_log_function, NULL);
    }


    // Set paths of input and output repos

    in_repo = g_strconcat(in_dir, "repodata/", NULL);

    if (cmd_options->outputdir) {
        out_dir = normalize_dir_path(cmd_options->outputdir);
        out_repo = g_strconcat(out_dir, "repodata/", NULL);
        tmp_out_repo = g_strconcat(out_dir, ".repodata/", NULL);
    } else {
        out_dir  = g_strdup(in_dir);
        out_repo = g_strdup(in_repo);
        tmp_out_repo = g_strconcat(out_dir, ".repodata/", NULL);
    }


    // Check if tmp_out_repo exists & Create tmp_out_repo dir

    if (g_mkdir (tmp_out_repo, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)) {
        if (errno == EEXIST) {
            g_critical("Temporary repodata directory: %s already exists! (Another createrepo process is running?)", tmp_out_repo);
        } else {
            g_critical("Error while creating temporary repodata directory %s: %s", tmp_out_repo, strerror(errno));
        }

        exit(1);
    }


    // Set handler for sigint

    tmp_repodata_path = tmp_out_repo;

    g_debug("SIGINT handler setup");
    struct sigaction sigact;
    sigact.sa_handler = sigint_catcher;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    if (sigaction(SIGINT, &sigact, NULL) == -1) {
        g_critical("sigaction(): %s", strerror(errno));
        exit(1);
    }



    // Copy groupfile

    gchar *groupfile = NULL;
    if (cmd_options->groupfile_fullpath) {
        groupfile = g_strconcat(tmp_out_repo, get_filename(cmd_options->groupfile_fullpath), NULL);
        g_debug("Copy groupfile %s -> %s", cmd_options->groupfile_fullpath, groupfile);
        if (better_copy_file(cmd_options->groupfile_fullpath, groupfile) != CR_COPY_OK) {
            g_critical("Error while copy %s -> %s", cmd_options->groupfile_fullpath, groupfile);
        }
    }


    // Load old metadata if --update

    GHashTable *old_metadata = NULL;
    if (cmd_options->update) {

        int ret;
        old_metadata = new_metadata_hashtable();

        // Load data from output dir if output dir is specified
        // This is default behaviour of classic createrepo
        if (cmd_options->outputdir) {
            ret = locate_and_load_xml_metadata(old_metadata, out_dir, HT_KEY_FILENAME);
            if (ret == LOAD_METADATA_OK) {
                g_debug("Old metadata from: %s - loaded", out_dir);
            } else {
                g_debug("Old metadata from %s - loading failed", out_dir);
            }
        }

        // Load local repodata
        // Classic createrepo doesn't load this metadata if --outputdir option is used,
        // but createrepo_c does.
        ret = locate_and_load_xml_metadata(old_metadata, in_dir, HT_KEY_FILENAME);
        if (ret == LOAD_METADATA_OK) {
            g_debug("Old metadata from: %s - loaded", in_dir);
        } else {
            g_debug("Old metadata from %s - loading failed", in_dir);
        }

        // Load repodata from --update-md-path
        GSList *element;
        for (element = cmd_options->l_update_md_paths; element; element = g_slist_next(element)) {
            char *path = (char *) element->data;
            g_message("Loading metadata from: %s", path);
            int ret = locate_and_load_xml_metadata(old_metadata, path, HT_KEY_FILENAME);
            if (ret == LOAD_METADATA_OK) {
                g_debug("Old metadata from md-path %s - loaded", path);
            } else {
                g_warning("Old metadata from md-path %s - loading failed", path);
            }
        }

        g_message("Loaded information about %d packages", g_hash_table_size(old_metadata));
    }


    // Setup compression types

    const char *sqlite_compression_suffix = NULL;
    CompressionType sqlite_compression = BZ2_COMPRESSION;
    CompressionType groupfile_compression = GZ_COMPRESSION;

    if (cmd_options->compression_type != UNKNOWN_COMPRESSION) {
        sqlite_compression    = cmd_options->compression_type;
        groupfile_compression = cmd_options->compression_type;
    }

    if (cmd_options->xz_compression) {
        sqlite_compression    = XZ_COMPRESSION;
        groupfile_compression = XZ_COMPRESSION;
    }

    sqlite_compression_suffix = get_suffix(sqlite_compression);


    // Create and open new compressed files

    CW_FILE *pri_cw_file;
    CW_FILE *fil_cw_file;
    CW_FILE *oth_cw_file;

    g_message("Temporary output repo path: %s", tmp_out_repo);
    g_debug("Creating .xml.gz files");

    gchar *pri_xml_filename = g_strconcat(tmp_out_repo, "/primary.xml.gz", NULL);
    gchar *fil_xml_filename = g_strconcat(tmp_out_repo, "/filelists.xml.gz", NULL);
    gchar *oth_xml_filename = g_strconcat(tmp_out_repo, "/other.xml.gz", NULL);

    if ((pri_cw_file = cw_open(pri_xml_filename, CW_MODE_WRITE, GZ_COMPRESSION)) == NULL) {
        g_critical("Cannot open file: %s", pri_xml_filename);
        g_free(pri_xml_filename);
        g_free(fil_xml_filename);
        g_free(oth_xml_filename);
        exit(1);
    }

    if ((fil_cw_file = cw_open(fil_xml_filename, CW_MODE_WRITE, GZ_COMPRESSION)) == NULL) {
        g_critical("Cannot open file: %s", fil_xml_filename);
        g_free(pri_xml_filename);
        g_free(fil_xml_filename);
        g_free(oth_xml_filename);
        cw_close(pri_cw_file);
        exit(1);
    }

    if ((oth_cw_file = cw_open(oth_xml_filename, CW_MODE_WRITE, GZ_COMPRESSION)) == NULL) {
        g_critical("Cannot open file: %s", oth_xml_filename);
        g_free(pri_xml_filename);
        g_free(fil_xml_filename);
        g_free(oth_xml_filename);
        cw_close(fil_cw_file);
        cw_close(pri_cw_file);
        exit(1);
    }


    // Open sqlite databases

    gchar *pri_db_filename = NULL;
    gchar *fil_db_filename = NULL;
    gchar *oth_db_filename = NULL;
    sqlite3 *pri_db = NULL;
    sqlite3 *fil_db = NULL;
    sqlite3 *oth_db = NULL;
    DbPrimaryStatements pri_statements   = NULL;
    DbFilelistsStatements fil_statements = NULL;
    DbOtherStatements oth_statements     = NULL;

    if (!cmd_options->no_database) {
        g_debug("Creating .xml.gz files");
        pri_db_filename = g_strconcat(tmp_out_repo, "/primary.sqlite", NULL);
        fil_db_filename = g_strconcat(tmp_out_repo, "/filelists.sqlite", NULL);
        oth_db_filename = g_strconcat(tmp_out_repo, "/other.sqlite", NULL);
        pri_db = open_primary_db(pri_db_filename, NULL);
        fil_db = open_filelists_db(fil_db_filename, NULL);
        oth_db = open_other_db(oth_db_filename, NULL);
        pri_statements = prepare_primary_db_statements(pri_db, NULL);
        fil_statements = prepare_filelists_db_statements(fil_db, NULL);
        oth_statements = prepare_other_db_statements(oth_db, NULL);
    }


    // Init package parser

    package_parser_init();


    // Thread pool - User data initialization

    struct UserData user_data;
    user_data.pri_f             = pri_cw_file;
    user_data.fil_f             = fil_cw_file;
    user_data.oth_f             = oth_cw_file;
    user_data.pri_statements    = pri_statements;
    user_data.fil_statements    = fil_statements;
    user_data.oth_statements    = oth_statements;
    user_data.changelog_limit   = cmd_options->changelog_limit;
    user_data.location_base     = cmd_options->location_base;
    user_data.checksum_type_str = get_checksum_name_str(cmd_options->checksum_type);
    user_data.checksum_type     = cmd_options->checksum_type;
    user_data.quiet             = cmd_options->quiet;
    user_data.verbose           = cmd_options->verbose;
    user_data.skip_symlinks     = cmd_options->skip_symlinks;
    user_data.skip_stat         = cmd_options->skip_stat;
    user_data.old_metadata      = old_metadata;
    user_data.repodir_name_len  = strlen(in_dir);

    g_debug("Thread pool user data ready");


    // Thread pool - Creation

    g_thread_init(NULL);
    GThreadPool *pool = g_thread_pool_new(dumper_thread, &user_data, 0, TRUE, NULL);

    g_debug("Thread pool ready");


    // Recursive walk

    int package_count = 0;

    if (!(cmd_options->include_pkgs)) {
        // --pkglist (or --includepkg) is not supplied -> do dir walk

        g_message("Directory walk started");

        size_t in_dir_len = strlen(in_dir);
        GStringChunk *sub_dirs_chunk = g_string_chunk_new(1024);
        GQueue *sub_dirs = g_queue_new();
        gchar *input_dir_stripped = g_string_chunk_insert_len(sub_dirs_chunk, in_dir, in_dir_len-1);
        g_queue_push_head(sub_dirs, input_dir_stripped);

        char *dirname;
        while ((dirname = g_queue_pop_head(sub_dirs))) {
            // Open dir
            GDir *dirp;
            dirp = g_dir_open (dirname, 0, NULL);
            if (!dirp) {
                g_warning("Cannot open directory: %s", dirname);
                continue;
            }

            const gchar *filename;
            while ((filename = g_dir_read_name(dirp))) {

                gchar *full_path = g_strconcat(dirname, "/", filename, NULL);

                // Non .rpm files
                if (!g_str_has_suffix (filename, ".rpm")) {
                    if (!g_file_test(full_path, G_FILE_TEST_IS_REGULAR) && 
                        g_file_test(full_path, G_FILE_TEST_IS_DIR))
                    {
                        // Directory
                        gchar *sub_dir_in_chunk = g_string_chunk_insert (sub_dirs_chunk, full_path);
                        g_queue_push_head(sub_dirs, sub_dir_in_chunk);
                        g_debug("Dir to scan: %s", sub_dir_in_chunk);
                    }
                    g_free(full_path);
                    continue;
                }

                // Skip symbolic links if --skip-symlinks arg is used
                if (cmd_options->skip_symlinks && g_file_test(full_path, G_FILE_TEST_IS_SYMLINK)) {
                    g_debug("Skipped symlink: %s", full_path);
                    g_free(full_path);
                    continue;
                }

                // Check filename against exclude glob masks
                const gchar *repo_relative_path = filename;
                if (in_dir_len < strlen(full_path))  // This probably should be always true
                    repo_relative_path = full_path + in_dir_len;

                if (allowed_file(repo_relative_path, cmd_options)) {
                    // FINALLY! Add file into pool
                    g_debug("Adding pkg: %s", full_path);
                    struct PoolTask *task = g_malloc(sizeof(struct PoolTask));
                    task->full_path = full_path;
                    task->filename = g_strdup(filename);
                    task->path = g_strdup(dirname);  // TODO: One common path for all tasks with the same path??
                    g_thread_pool_push(pool, task, NULL);
                    package_count++;
                }
            }

            // Cleanup
            g_dir_close (dirp);
        }

        g_string_chunk_free (sub_dirs_chunk);
        g_queue_free(sub_dirs);
    } else {
        // pkglist is supplied - use only files in pkglist

        g_debug("Skipping dir walk - using pkglist");

        GSList *element;
        for (element=cmd_options->include_pkgs; element; element=g_slist_next(element)) {
            gchar *relative_path = (gchar *) element->data;   // path from pkglist e.g. packages/i386/foobar.rpm
            gchar *full_path = g_strconcat(in_dir, relative_path, NULL);   // /path/to/in_repo/packages/i386/foobar.rpm
            gchar *dirname;  // packages/i386/
            gchar *filename;  // foobar.rpm

            // Get index of last '/'
            int rel_path_len = strlen(relative_path);
            int x = rel_path_len;
            for (; x > 0; x--) {
                if (relative_path[x] == '/') {
                    break;
                }
            }

            if (!x) {
                // There was no '/' in path
                filename = relative_path;
            } else {
                filename = relative_path + x + 1;
            }
            dirname  = strndup(relative_path, x);

            if (allowed_file(filename, cmd_options)) {
                // Check filename against exclude glob masks
                g_debug("Adding pkg: %s", full_path);
                struct PoolTask *task = g_malloc(sizeof(struct PoolTask));
                task->full_path = full_path;
                task->filename = g_strdup(filename);
                task->path = dirname;
                g_thread_pool_push(pool, task, NULL);
                package_count++;
            }
        }
    }

    g_debug("Package count: %d", package_count);
    g_message("Directory walk done");


    // Write XML header

    g_debug("Writing xml headers");

    cw_printf(user_data.pri_f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<metadata xmlns=\""XML_COMMON_NS"\" xmlns:rpm=\""XML_RPM_NS"\" packages=\"%d\">\n", package_count);
    cw_printf(user_data.fil_f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<filelists xmlns=\""XML_FILELISTS_NS"\" packages=\"%d\">\n", package_count);
    cw_printf(user_data.oth_f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<otherdata xmlns=\""XML_OTHER_NS"\" packages=\"%d\">\n", package_count);


    // Start pool

    user_data.package_count = package_count;
    g_thread_pool_set_max_threads(pool, cmd_options->workers, NULL);
    g_message("Pool started (with %d workers)", cmd_options->workers);


    // Wait until pool is finished

    g_thread_pool_free(pool, FALSE, TRUE);
    g_message("Pool finished");

    cw_puts(user_data.pri_f, "</metadata>");
    cw_puts(user_data.fil_f, "</filelists>");
    cw_puts(user_data.oth_f, "</otherdata>");

    cw_close(user_data.pri_f);
    cw_close(user_data.fil_f);
    cw_close(user_data.oth_f);


    // Close db

    destroy_primary_db_statements(user_data.pri_statements);
    destroy_filelists_db_statements(user_data.fil_statements);
    destroy_other_db_statements(user_data.oth_statements);

    close_primary_db(pri_db, NULL);
    close_filelists_db(fil_db, NULL);
    close_other_db(oth_db, NULL);


    // Move files from out_repo into tmp_out_repo

    g_debug("Moving data from %s", out_repo);
    if (g_file_test(out_repo, G_FILE_TEST_EXISTS)) {

        // Delete old metadata
        g_debug("Removing old metadata from %s", out_repo);
        remove_metadata(out_dir);

        // Move files from out_repo to tmp_out_repo
        GDir *dirp;
        dirp = g_dir_open (out_repo, 0, NULL);
        if (!dirp) {
            g_critical("Cannot open directory: %s", out_repo);
            exit(1);
        }

        const gchar *filename;
        while ((filename = g_dir_read_name(dirp))) {
            gchar *full_path = g_strconcat(out_repo, filename, NULL);
            gchar *new_full_path = g_strconcat(tmp_out_repo, filename, NULL);

            if (g_rename(full_path, new_full_path) == -1) {
                g_critical("Cannot move file %s -> %s", full_path, new_full_path);
            } else {
                g_debug("Moved %s -> %s", full_path, new_full_path);
            }

            g_free(full_path);
            g_free(new_full_path);
        }

        g_dir_close(dirp);

        // Remove out_repo
        if (g_rmdir(out_repo) == -1) {
            g_critical("Cannot remove %s", out_repo);
        } else {
            g_debug("Old out repo %s removed", out_repo);
        }
    }


    // Rename tmp_out_repo to out_repo
    if (g_rename(tmp_out_repo, out_repo) == -1) {
        g_critical("Cannot rename %s -> %s", tmp_out_repo, out_repo);
    } else {
        g_debug("Renamed %s -> %s", tmp_out_repo, out_repo);
    }


    // Create repomd records for each file

    g_debug("Generating repomd.xml");

    RepomdRecord pri_xml_rec = new_repomdrecord("repodata/primary.xml.gz");
    RepomdRecord fil_xml_rec = new_repomdrecord("repodata/filelists.xml.gz");
    RepomdRecord oth_xml_rec = new_repomdrecord("repodata/other.xml.gz");
    RepomdRecord pri_db_rec               = NULL;
    RepomdRecord fil_db_rec               = NULL;
    RepomdRecord oth_db_rec               = NULL;
    RepomdRecord groupfile_rec            = NULL;
    RepomdRecord compressed_groupfile_rec = NULL;


    // XML

    fill_missing_data(out_dir, pri_xml_rec, &(cmd_options->checksum_type));
    fill_missing_data(out_dir, fil_xml_rec, &(cmd_options->checksum_type));
    fill_missing_data(out_dir, oth_xml_rec, &(cmd_options->checksum_type));


    // Groupfile

    if (groupfile) {
        gchar *groupfile_name = g_strconcat("repodata/", get_filename(groupfile), NULL);
        groupfile_rec = new_repomdrecord(groupfile_name);
        compressed_groupfile_rec = new_repomdrecord(groupfile_name);

        process_groupfile(out_dir, groupfile_rec, compressed_groupfile_rec,
                      &(cmd_options->checksum_type), groupfile_compression);
        g_free(groupfile_name);
    }


    // Sqlite db

    if (!cmd_options->no_database) {
        gchar *pri_db_name = g_strconcat("repodata/primary.sqlite", sqlite_compression_suffix, NULL);
        gchar *fil_db_name = g_strconcat("repodata/filelists.sqlite", sqlite_compression_suffix, NULL);
        gchar *oth_db_name = g_strconcat("repodata/other.sqlite", sqlite_compression_suffix, NULL);

        gchar *tmp_pri_db_path;
        gchar *tmp_fil_db_path;
        gchar *tmp_oth_db_path;


        // Open dbs again (but from the new (final) location)
        // and insert XML checksums

        tmp_pri_db_path = g_strconcat(out_dir, "repodata/primary.sqlite", NULL);
        tmp_fil_db_path = g_strconcat(out_dir, "repodata/filelists.sqlite", NULL);
        tmp_oth_db_path = g_strconcat(out_dir, "repodata/other.sqlite", NULL);

        sqlite3_open(tmp_pri_db_path, &pri_db);
        sqlite3_open(tmp_fil_db_path, &fil_db);
        sqlite3_open(tmp_oth_db_path, &oth_db);

        dbinfo_update(pri_db, pri_xml_rec->checksum, NULL);
        dbinfo_update(fil_db, fil_xml_rec->checksum, NULL);
        dbinfo_update(oth_db, oth_xml_rec->checksum, NULL);

        sqlite3_close(pri_db);
        sqlite3_close(fil_db);
        sqlite3_close(oth_db);


        // Compress dbs

        compress_file(tmp_pri_db_path, NULL, sqlite_compression);
        compress_file(tmp_fil_db_path, NULL, sqlite_compression);
        compress_file(tmp_oth_db_path, NULL, sqlite_compression);

        remove(tmp_pri_db_path);
        remove(tmp_fil_db_path);
        remove(tmp_oth_db_path);

        g_free(tmp_pri_db_path);
        g_free(tmp_fil_db_path);
        g_free(tmp_oth_db_path);


        // Prepare repomd records

        pri_db_rec = new_repomdrecord(pri_db_name);
        fil_db_rec = new_repomdrecord(fil_db_name);
        oth_db_rec = new_repomdrecord(oth_db_name);

        fill_missing_data(out_dir, pri_db_rec, &(cmd_options->checksum_type));
        fill_missing_data(out_dir, fil_db_rec, &(cmd_options->checksum_type));
        fill_missing_data(out_dir, oth_db_rec, &(cmd_options->checksum_type));

        g_free(pri_db_name);
        g_free(fil_db_name);
        g_free(oth_db_name);
    }


    // Add checksums into files names

    if (cmd_options->unique_md_filenames) {
        rename_file(out_dir, pri_xml_rec);
        rename_file(out_dir, fil_xml_rec);
        rename_file(out_dir, oth_xml_rec);
        rename_file(out_dir, pri_db_rec);
        rename_file(out_dir, fil_db_rec);
        rename_file(out_dir, oth_db_rec);
        rename_file(out_dir, groupfile_rec);
        rename_file(out_dir, compressed_groupfile_rec);
    }


    // Gen xml

    char *repomd_xml = xml_repomd(out_dir, pri_xml_rec, fil_xml_rec,
                                  oth_xml_rec, pri_db_rec, fil_db_rec,
                                  oth_db_rec, groupfile_rec,
                                  compressed_groupfile_rec, NULL);
    gchar *repomd_path = g_strconcat(out_repo, "repomd.xml", NULL);


    // Write repomd.xml

    FILE *frepomd = fopen(repomd_path, "w");
    if (!frepomd || !repomd_xml) {
        g_critical("Generate of repomd.xml failed");
        return 1;
    }
    fputs(repomd_xml, frepomd);
    fclose(frepomd);
    g_free(repomd_xml);
    g_free(repomd_path);

    free_repomdrecord(pri_xml_rec);
    free_repomdrecord(fil_xml_rec);
    free_repomdrecord(oth_xml_rec);
    free_repomdrecord(pri_db_rec);
    free_repomdrecord(fil_db_rec);
    free_repomdrecord(oth_db_rec);
    free_repomdrecord(groupfile_rec);
    free_repomdrecord(compressed_groupfile_rec);


    // Clean up

    g_debug("Memory cleanup");

    if (old_metadata) {
        destroy_metadata_hashtable(old_metadata);
    }

    g_free(in_repo);
    g_free(out_repo);
    tmp_repodata_path = NULL;
    g_free(tmp_out_repo);
    g_free(in_dir);
    g_free(out_dir);
    g_free(pri_xml_filename);
    g_free(fil_xml_filename);
    g_free(oth_xml_filename);
    g_free(pri_db_filename);
    g_free(fil_db_filename);
    g_free(oth_db_filename);
    g_free(groupfile);

    free_options(cmd_options);
    package_parser_shutdown();

    g_debug("All done");
    return 0;
}
