#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <sys/wait.h>
#include <sys/limits.h>
#include <dirent.h>
#include <sys/stat.h>

#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>

#include "bootloader.h"
#include "common.h"
extern "C" {	
#include "libcrecovery/common.h"
#include "flashutils/flashutils.h"
#include "mtdutils/mounts.h"
#include "yaffs2_static/mkyaffs2image.h"
#include "yaffs2_static/unyaffs.h"
}

#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minzip/DirUtil.h"
#include "roots.h"
#include "recovery_ui.h"
#include <sys/vfs.h>
#include "nandroid.h"

#include "miui_func.hpp"
#include "utils_func.hpp"

#define MIUI_RECOVERY "miui_recovery"

#define STATE_MD5 "/sdcard/miui_recovery/backup/.md5_state"

static bool enable_md5 = true; //default is true
static void refresh_md5_check_state();

static void ensure_directory(const char* dir);

void nandroid_generate_timestamp_path(char* backup_path)
{
    time_t t = time(NULL);
    struct tm *tmp = localtime(&t);
    if (tmp == NULL)
    {
        struct timeval tp;
        gettimeofday(&tp, NULL);
        sprintf(backup_path, "/sdcard/miui_recovery/backup/%ld", tp.tv_sec);
    }
    else
    {
        strftime(backup_path, PATH_MAX, "/sdcard/miui_recovery/backup/%F.%H.%M.%S", tmp);
    }
}

static int print_and_error(const char* message) {
    ui_print("%s", message);
    return 1;
}

static long delta_milliseconds(struct timeval from, struct timeval to) {
  long delta_sec = (to.tv_sec - from.tv_sec)*1000;
  long delta_usec = (to.tv_usec - from.tv_usec)/1000;
  return (delta_sec + delta_usec);
}

/*
 * How often nandroid updates text in ms
 */

#define NANDROID_UPDATE_INTERVAL 1000

//this is for dedupe
static int nandroid_backup_bitfield = 0;
#define NANDROID_FIELD_DEDUPE_CLEARED_SPACE 1

static struct timeval lastupdate = (struct timeval) {0};
static int yaffs_files_total = 0;
static int yaffs_files_count = 0;
static void yaffs_callback(char* filename)
{
    if (filename == NULL)
        return;
    const char* justfile = basename(filename);
    char tmp[PATH_MAX];
    struct timeval curtime;
    gettimeofday(&curtime,NULL);
    /*
     * Only update once every NANDROID_UPDATE_INTERVAL
     * milli seconds.  We don't need frequent progress
     * updates and updating every file uses WAY
     * too much CPU time.
     */
    yaffs_files_count++;
    if(delta_milliseconds(lastupdate,curtime) > NANDROID_UPDATE_INTERVAL)
      {
        strcpy(tmp, justfile);
        if (tmp[strlen(tmp) - 1] == '\n')
          tmp[strlen(tmp) - 1] = '\0';
        if (strlen(tmp) < 30) {
        lastupdate = curtime;
          ui_print("%s", tmp);
	}

        if (yaffs_files_total != 0)
          ui_set_progress((float)yaffs_files_count / (float)yaffs_files_total);
      }
}

static void compute_directory_stats(const char* directory)
{
    char tmp[PATH_MAX];
    sprintf(tmp, "find %s | wc -l > /tmp/dircount", directory);
    __system(tmp);
    char count_text[100];
    FILE* f = fopen("/tmp/dircount", "r");
    fread(count_text, 1, sizeof(count_text), f);
    fclose(f);
    yaffs_files_count = 0;
    yaffs_files_total = atoi(count_text);
    ui_reset_progress();
    ui_show_progress(1, 0);
}

typedef void (*file_event_callback)(const char* filename);
typedef int (*nandroid_backup_handler)(const char* backup_path, const char* backup_file_image, int callback);

static int mkyaffs2image_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    snprintf(tmp, PATH_MAX, "cd %s ; mkyaffs2image . %s.img ; exit $?", backup_path, backup_file_image);
    FILE *fp = __popen(tmp, "r");
     if (NULL == fp) {
	     ui_print("Unable to execute mkyaffs2image.\n");
	     return -1;
     }
     while (fgets(tmp, PATH_MAX, fp) != NULL) {
	     tmp[PATH_MAX -1] = '\0';
	      if (callback)
		      yaffs_callback(tmp);
     }

     return __pclose(fp);
}

static int do_tar_compress(char* command, int callback) {
	char buf[PATH_MAX];

    FILE* fp = __popen(command, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar command!\n");
        return -1;
    }

    while (fgets(buf, PATH_MAX, fp) != NULL) {
        buf[PATH_MAX - 1] = '\0';
        if (callback)
            yaffs_callback(buf);
    }

    return __pclose(fp);
}

static int tar_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; touch %s.tar ; (tar cv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) | split -a 1 -b 1000000000 /proc/self/fd/0 %s.tar.) 2> /proc/self/fd/1 ; exit $?", backup_path, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path, backup_file_image);

    return do_tar_compress(tmp, callback);
}


static int tar_gzip_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; touch %s.tar.gz ; (tar cv --exclude=data/data/com.google.android.music/files/* %s $(basename %s) | pigz -c | split -a 1 -b 1000000000 /proc/self/fd/0 %s.tar.gz.) 2> /proc/self/fd/1 ; exit $?", backup_path, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "--exclude 'media'" : "", backup_path, backup_file_image);

    return do_tar_compress(tmp, callback);
}

//BACKUP METHOD OF DEDUPE
void nandroid_dedupe_gc(const char* blob_dir) {
    char backup_dir[PATH_MAX];
    strcpy(backup_dir, blob_dir);
    char *d = dirname(backup_dir);
    strcpy(backup_dir, d);
    //remove this line ,because the backup path is "/sdcard/miui_recovery/backup"
   // strcat(backup_dir, "/backup"); 
    ui_print("Freeing space...\n");
    char tmp[PATH_MAX];
    sprintf(tmp, "dedupe gc %s $(find %s -name '*.dup')", blob_dir, backup_dir);
    __system(tmp);
    ui_print("Done freeing space.\n");
}


   static int dedupe_compress_wrapper(const char* backup_path, const char* backup_file_image, int callback) {
    char tmp[PATH_MAX];
    char blob_dir[PATH_MAX];
    strcpy(blob_dir, backup_file_image);
    char *d = dirname(blob_dir);
    strcpy(blob_dir, d);
    d = dirname(blob_dir);
    strcpy(blob_dir, d);
    d = dirname(blob_dir);
    strcpy(blob_dir, d);
    strcat(blob_dir, "/blobs");
    ensure_directory(blob_dir);

    if (!(nandroid_backup_bitfield & NANDROID_FIELD_DEDUPE_CLEARED_SPACE)) {
        nandroid_backup_bitfield |= NANDROID_FIELD_DEDUPE_CLEARED_SPACE;
        nandroid_dedupe_gc(blob_dir);
    }

    sprintf(tmp, "dedupe c %s %s %s.dup %s", backup_path, blob_dir, backup_file_image, strcmp(backup_path, "/data") == 0 && is_data_media() ? "./media" : "");

    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute dedupe.\n");
        return -1;
    }

    while (fgets(tmp, PATH_MAX, fp) != NULL) {
        tmp[PATH_MAX - 1] = '\0';
        if (callback)
            yaffs_callback(tmp);
    }

    return __pclose(fp);
 }

/* End of backup method dedupe */

static nandroid_backup_handler default_backup_handler = tar_compress_wrapper;
static char forced_backup_format[5] = "";
void nandroid_force_backup_format(const char* fmt) {
    strcpy(forced_backup_format, fmt);
}


static void refresh_md5_check_state() {
	char fmt[5];
//	miuiIntent_sent(INTENT_MOUNT, 1, "/sdcard");
	ensure_path_mounted("/sdcard");
	FILE *f = fopen(STATE_MD5, "r");
	if (NULL == f) {
		enable_md5 = true;
		return;
	}
	fread(fmt, 1, sizeof(fmt), f);
	fclose(f);

	if (0 == strcmp(fmt, "off")) {
		enable_md5 = false;
	}  else {
		enable_md5 = true;
	}
}


static void refresh_default_backup_handler() {
    char fmt[5];
    if (strlen(forced_backup_format) > 0) {
        strcpy(fmt, forced_backup_format);
    }
    else {
        ensure_path_mounted("/sdcard");
        FILE* f = fopen(NANDROID_BACKUP_FORMAT_FILE, "r");
        if (NULL == f) {
            default_backup_handler = tar_compress_wrapper;
            return;
        }
        fread(fmt, 1, sizeof(fmt), f);
        fclose(f);
    }
    fmt[3] = '\0';
    if (0 == strcmp(fmt, "dup")) {
        default_backup_handler = dedupe_compress_wrapper;
     } else if (0 == strcmp(fmt, "tar")) {
        default_backup_handler = tar_compress_wrapper;
     } else if (0 == strcmp(fmt, "tgz")) {
	     default_backup_handler = tar_gzip_compress_wrapper;
     } else {
	     default_backup_handler = tar_compress_wrapper;
     }
}

unsigned nandroid_get_default_backup_format() {
    refresh_default_backup_handler();
    if (default_backup_handler == dedupe_compress_wrapper) {
        return NANDROID_BACKUP_FORMAT_DUP;
    } else if (default_backup_handler == tar_gzip_compress_wrapper) {
	    return NANDROID_BACKUP_FORMAT_TGZ;
    } else {
        return NANDROID_BACKUP_FORMAT_TAR;
    }
}

static nandroid_backup_handler get_backup_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    refresh_default_backup_handler();
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
     MountedVolume *mv = (MountedVolume*)find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
          return default_backup_handler;
    }

    // cwr5, we prefer tar for everything except yaffs2
    if (strcmp("yaffs2", mv->filesystem) == 0) {
        return mkyaffs2image_wrapper;
    }

    char str[255];
    char* partition;
    property_get("ro.cwm.prefer_dedupe", str, "true");
    if (strcmp("true", str) != 0) {
        return mkyaffs2image_wrapper;
    }

   
      return default_backup_handler;
}


static int nandroid_backup_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    struct stat file_info;
    int callback = stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info) != 0;

    ui_print("Backing up %s...\n", name);
    if (0 != (ret = ensure_path_mounted(mount_point) != 0)) {
        ui_print("Can't mount %s!\n", mount_point);
        return ret;
    }
    compute_directory_stats(mount_point);
    char tmp[PATH_MAX];
    scan_mounted_volumes();
    Volume *v = volume_for_path(mount_point);
    const MountedVolume *mv = NULL;
    if (v != NULL)
        mv = (MountedVolume*)find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL || mv->filesystem == NULL)
        sprintf(tmp, "%s/%s.auto", backup_path, name);
    else
        sprintf(tmp, "%s/%s.%s", backup_path, name, mv->filesystem);
    nandroid_backup_handler backup_handler = get_backup_handler(mount_point);
    if (backup_handler == NULL) {
        ui_print("Error finding an appropriate backup handler.\n");
        return -2;
    }
    ret = backup_handler(mount_point, tmp, callback);
    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    if (0 != ret) {
        ui_print("Error while making a backup image of %s!\n", mount_point);
        return ret;
    }
    return 0;
}

static int nandroid_backup_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists before attempting anything...
    if (vol == NULL || vol->fs_type == NULL)
        return NULL;

    // see if we need a raw backup (mtd)
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", backup_path);
    __system(tmp);
    int ret;
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        const char* name = basename(root);
        sprintf(tmp, "%s/%s.img", backup_path, name);
        ui_print("Backing up %s image...\n", name);
        if (0 != (ret = backup_raw_partition(vol->fs_type, vol->device, tmp))) {
            ui_print("Error while backing up %s image!", name);
            return ret;
        }
        return 0;
    }

    return nandroid_backup_partition_extended(backup_path, root, 1);
}

extern "C" int nandroid_advanced_backup(const char* backup_path, const char *root)
{

      utils Utils;
    if (ensure_path_mounted(backup_path) != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    
    Volume* volume = volume_for_path(backup_path);
    if (NULL == volume) {
      if (strstr(backup_path, "/sdcard") == backup_path && is_data_media())
          volume = volume_for_path("/data");
      else
          return print_and_error("Unable to find volume for backup path.\n");
    }
    int ret;
    struct statfs s;
    if (NULL != volume) {
        if (0 != (ret = statfs(volume->mount_point, &s)))
            return print_and_error("Unable to stat backup path.\n");
        uint64_t bavail = s.f_bavail;
        uint64_t bsize = s.f_bsize;
        uint64_t sdcard_free = bavail * bsize;
        uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
        ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
        if (sdcard_free_mb < 150)
            ui_print("There may not be enough free space to complete backup... continuing...\n");
    }
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", backup_path);
    __system(tmp);

    if (0 != (ret = nandroid_backup_partition(backup_path, root)))
        return ret;
    //Utils.get_file_in_folder(backup_path);
    //refresh_md5_check_state(); // on or off 
   // if (enable_md5) {
    Utils.Make_MD5(backup_path);
	  //  utils::Make_MD5(backup_path);
   // }

    sync();
    ui_print("\nBackup complete!\n");
    return 0;

}

int nandroid_backup(const char* backup_path)
{
	utils Utils;
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    refresh_default_backup_handler();
    
    if (ensure_path_mounted(backup_path) != 0) {
        return print_and_error("Can't mount backup path.\n");
    }
    
    Volume* volume = volume_for_path(backup_path);
    if (NULL == volume) {
      if (strstr(backup_path, "/sdcard") == backup_path && is_data_media())
          volume = volume_for_path("/data");
      else
          return print_and_error("Unable to find volume for backup path.\n");
    }
    int ret;
    struct statfs s;
    if (NULL != volume) {
        if (0 != (ret = statfs(volume->mount_point, &s)))
            return print_and_error("Unable to stat backup path.\n");
        uint64_t bavail = s.f_bavail;
        uint64_t bsize = s.f_bsize;
        uint64_t sdcard_free = bavail * bsize;
        uint64_t sdcard_free_mb = sdcard_free / (uint64_t)(1024 * 1024);
        ui_print("SD Card space free: %lluMB\n", sdcard_free_mb);
        if (sdcard_free_mb < 150)
            ui_print("There may not be enough free space to complete backup... continuing...\n");
    }
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", backup_path);
    __system(tmp);

    if (0 != (ret = nandroid_backup_partition(backup_path, "/boot")))
        return ret;

    if (0 != (ret = nandroid_backup_partition(backup_path, "/recovery")))
        return ret;

    Volume *vol = volume_for_path("/wimax");
    if (vol != NULL && 0 == statfs(vol->device, &s))
    {
        char serialno[PROPERTY_VALUE_MAX];
        ui_print("Backing up WiMAX...\n");
        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);
        ret = backup_raw_partition(vol->fs_type, vol->device, tmp);
        if (0 != ret)
            return print_and_error("Error while dumping WiMAX image!\n");
    }

    if (0 != (ret = nandroid_backup_partition(backup_path, "/system")))
        return ret;

    if (0 != (ret = nandroid_backup_partition(backup_path, "/data")))
        return ret;

    if (has_datadata()) {
        if (0 != (ret = nandroid_backup_partition(backup_path, "/datadata")))
            return ret;
    }

    if (0 != statfs("/sdcard/.android_secure", &s))
    {
        ui_print("No /sdcard/.android_secure found. Skipping backup of applications on external storage.\n");
    }
    else
    {
        if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
            return ret;
    }

    if (0 != (ret = nandroid_backup_partition_extended(backup_path, "/cache", 0)))
        return ret;

    vol = volume_for_path("/sd-ext");
    if (vol == NULL || 0 != statfs(vol->device, &s))
    {
        ui_print("No sd-ext found. Skipping backup of sd-ext.\n");
    }
    else
    {
        if (0 != ensure_path_mounted("/sd-ext"))
            ui_print("Could not mount sd-ext. sd-ext backup may not be supported on this device. Skipping backup of sd-ext.\n");
        else if (0 != (ret = nandroid_backup_partition(backup_path, "/sd-ext")))
            return ret;
    }
    // Utils.get_file_in_folder(backup_path);
    //refresh_md5_check_state(); // on or off
    // if (enable_md5) {
    Utils.Make_MD5(backup_path);
    // }
    //ui_print("Generating md5 sum...\n");
    //sprintf(tmp, "nandroid-md5.sh %s", backup_path);
    //if (0 != (ret = __system(tmp))) {
     //   ui_print("Error while generating md5 sum!\n");
      //  return ret;
   // }
    
    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
    ui_print("\nBackup complete!\n");
    return 0;
}

typedef int (*format_function)(char* root);

static void ensure_directory(const char* dir) {
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p %s", dir);
    __system(tmp);
}

typedef int (*nandroid_restore_handler)(const char* backup_file_image, const char* backup_path, int callback);

static int unyaffs_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    //gettimeofday(&lastupdate,NULL);
    //return unyaffs((char*)backup_file_image, (char*)backup_path, callback ? yaffs_callback : NULL);
    char tmp[PATH_MAX];
    snprintf(tmp, PATH_MAX, "cd %s ; unyaffs %s ; exit $?", backup_path, backup_file_image);
    FILE *fp = __popen(tmp, "r");
    if (NULL == fp) {
	    ui_print("Unable to execute unyaffs.\n");
	    return -1;
    }
    while (fgets(tmp, PATH_MAX, fp) != NULL) {
	    tmp[PATH_MAX -1] = '\0';
	     if (callback) 
		     yaffs_callback(tmp);
    }
    return __pclose(fp);
}

static int do_tar_extract(char* command, int callback) {
	char buf[PATH_MAX];
    FILE *fp = __popen(command, "r");
    if (fp == NULL) {
        ui_print("Unable to execute tar command.\n");
        return -1;
    }

    while (fgets(buf, PATH_MAX, fp) != NULL) {
        buf[PATH_MAX - 1] = '\0';
        if (callback)
            yaffs_callback(buf);
    }

    return __pclose(fp);
}


static int tar_gzip_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; cat %s* | pigz -d -c | tar xv ; exit $?", backup_path, backup_file_image);

    return do_tar_extract(tmp, callback);
}

static int tar_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    sprintf(tmp, "cd $(dirname %s) ; cat %s* | tar xv ; exit $?", backup_path, backup_file_image);

    return do_tar_extract(tmp, callback);
}


/* Restore method of dedupe */
/* Begin dedupe */
static int dedupe_extract_wrapper(const char* backup_file_image, const char* backup_path, int callback) {
    char tmp[PATH_MAX];
    char blob_dir[PATH_MAX];
    strcpy(blob_dir, backup_file_image);
    char *bd = dirname(blob_dir);
    strcpy(blob_dir, bd);
    bd = dirname(blob_dir);
    strcpy(blob_dir, bd);
    bd = dirname(blob_dir);
    sprintf(tmp, "dedupe x %s %s/blobs %s; exit $?", backup_file_image, bd, backup_path);

    char path[PATH_MAX];
    FILE *fp = __popen(tmp, "r");
    if (fp == NULL) {
        ui_print("Unable to execute dedupe.\n");
        return -1;
    }

    while (fgets(path, PATH_MAX, fp) != NULL) {
        if (callback)
            yaffs_callback(path);
    }

    return __pclose(fp);
}
/* End of dedupe */

static nandroid_restore_handler get_restore_handler(const char *backup_path) {
    Volume *v = volume_for_path(backup_path);
    if (v == NULL) {
        ui_print("Unable to find volume.\n");
        return NULL;
    }
    scan_mounted_volumes();
  const  MountedVolume *mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv == NULL) {
        ui_print("Unable to find mounted volume: %s\n", v->mount_point);
        return NULL;
    }

    if (strcmp(backup_path, "/data") == 0 && is_data_media()) {
  
	  return tar_extract_wrapper;
    }

    // cwr 5, we prefer tar for everything unless it is yaffs2
    char str[255];
    char* partition;
    property_get("ro.cwm.prefer_dedupe", str, "false");
    if (strcmp("true", str) != 0) {
        return unyaffs_wrapper;
    }

    if (strcmp("yaffs2", mv->filesystem) == 0) {
        return unyaffs_wrapper;
    }

   
      return tar_extract_wrapper;
}

static int nandroid_restore_partition_extended(const char* backup_path, const char* mount_point, int umount_when_finished) {
    int ret = 0;
    char* name = basename(mount_point);

    nandroid_restore_handler restore_handler = NULL;
    const char *filesystems[] = { "yaffs2", "ext2", "ext3", "ext4", "vfat", "rfs", NULL };
    const char* backup_filesystem = NULL;
    Volume *vol = volume_for_path(mount_point);
    const char *device = NULL;
    if (vol != NULL)
        device = vol->device;

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/%s.img", backup_path, name);
    struct stat file_info;
    if (0 != (ret = stat(tmp, &file_info))) {
        // can't find the backup, it may be the new backup format?
        // iterate through the backup types
        printf("couldn't find default\n");
        char *filesystem;
        int i = 0;
        while ((filesystem = (char*)filesystems[i]) != NULL) {
            sprintf(tmp, "%s/%s.%s.img", backup_path, name, filesystem);
            if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = unyaffs_wrapper;
                break;
            }

	    sprintf(tmp, "%s/%s.%s.tar", backup_path, name, filesystem);
	    if (0 == (ret = stat(tmp, &file_info))) {
		    backup_filesystem = filesystem;
		    restore_handler = tar_extract_wrapper;
		    break;
	    }

	    sprintf(tmp, "%s/%s.%s.tar.gz",backup_path, name, filesystem);
	    if (0 == (ret = stat(tmp, &file_info))) {
		    backup_filesystem = filesystem;
		    restore_handler = tar_gzip_extract_wrapper;
		    break;
	    }


            sprintf(tmp, "%s/%s.%s.dup", backup_path, name, filesystem);
           // if (0 == (ret = statfs(tmp, &file_info))) {
	   if (0 == (ret = stat(tmp, &file_info))) {
                backup_filesystem = filesystem;
                restore_handler = dedupe_extract_wrapper;
                break;
            }
            i++;
        }

        if (backup_filesystem == NULL || restore_handler == NULL) {
            ui_print("%s.img not found. Skipping restore of %s.\n", name, mount_point);
            return 0;
        }
        else {
            printf("Found new backup image: %s\n", tmp);
        }

        // If the fs_type of this volume is "auto" or mount_point is /data
        // and is_data_media (redundantly, and vol for /sdcard is NULL), let's revert
        // to using a rm -rf, rather than trying to do a
        // ext3/ext4/whatever format.
        // This is because some phones (like DroidX) will freak out if you
        // reformat the /system or /data partitions, and not boot due to
        // a locked bootloader.
        // Other devices, like the Galaxy Nexus, XOOM, and Galaxy Tab 10.1
        // have a /sdcard symlinked to /data/media. /data is set to "auto"
        // so that when the format occurs, /data/media is not erased.
        // The "auto" fs type preserves the file system, and does not
        // trigger that lock.
        // Or of volume does not exist (.android_secure), just rm -rf.
        if (vol == NULL || 0 == strcmp(vol->fs_type, "auto"))
            backup_filesystem = NULL;
        else if (0 == strcmp(vol->mount_point, "/data") && volume_for_path("/sdcard") == NULL && is_data_media())
	         backup_filesystem = NULL;
    }

    ensure_directory(mount_point);

    int callback = stat("/sdcard/clockworkmod/.hidenandroidprogress", &file_info) != 0;

    ui_print("Restoring %s...\n", name);
    if (backup_filesystem == NULL) {
        if (0 != (ret = format_volume(mount_point))) {
            ui_print("Error while formatting %s!\n", mount_point);
            return ret;
        }
    }
    else if (0 != (ret = format_device(device, mount_point, backup_filesystem))) {
        ui_print("Error while formatting %s!\n", mount_point);
        return ret;
    }

    if (0 != (ret = ensure_path_mounted(mount_point))) {
        ui_print("Can't mount %s!\n", mount_point);
        return ret;
    }

    if (restore_handler == NULL)
        restore_handler = get_restore_handler(mount_point);
    if (restore_handler == NULL) {
        ui_print("Error finding an appropriate restore handler.\n");
        return -2;
    }
    if (0 != (ret = restore_handler(tmp, mount_point, callback))) {
        ui_print("Error while restoring %s!\n", mount_point);
        return ret;
    }

    if (umount_when_finished) {
        ensure_path_unmounted(mount_point);
    }
    
    return 0;
}

static int nandroid_restore_partition(const char* backup_path, const char* root) {
    Volume *vol = volume_for_path(root);
    // make sure the volume exists...
    if (vol == NULL || vol->fs_type == NULL)
        return 0;

    // see if we need a raw restore (mtd)
    char tmp[PATH_MAX];
    if (strcmp(vol->fs_type, "mtd") == 0 ||
            strcmp(vol->fs_type, "bml") == 0 ||
            strcmp(vol->fs_type, "emmc") == 0) {
        int ret;
        const char* name = basename(root);
        ui_print("Erasing %s before restore...\n", name);
        if (0 != (ret = format_volume(root))) {
            ui_print("Error while erasing %s image!", name);
            return ret;
        }
        sprintf(tmp, "%s%s.img", backup_path, root);
        ui_print("Restoring %s image...\n", name);
        if (0 != (ret = restore_raw_partition(vol->fs_type, vol->device, tmp))) {
            ui_print("Error while flashing %s image!", name);
            return ret;
        }
        return 0;
    }
    return nandroid_restore_partition_extended(backup_path, root, 1);
}

int nandroid_restore(const char* backup_path, int restore_boot, int restore_system, int restore_data, int restore_cache, int restore_sdext, int restore_wimax)
{
    
	utils Utils;
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    yaffs_files_total = 0;

    if (ensure_path_mounted(backup_path) != 0)
        return print_and_error("Can't mount backup path\n");

    char tmp[PATH_MAX];

   // ui_print("Checking MD5 sums...\n");
   // sprintf(tmp, "cd %s && md5sum -c nandroid.md5", backup_path);
   // if (0 != __system(tmp))
      //  return print_and_error("MD5 mismatch!\n");
   
    int ret; 
   // refresh_md5_check_state();// on or off
   // if (enable_md5) {

     if(!Utils.Check_MD5(backup_path)) // 
	     return print_and_error("MD5 mismatch!\n");

   
    if (restore_boot && NULL != volume_for_path("/boot") && 0 != (ret = nandroid_restore_partition(backup_path, "/boot")))
        return ret;
    
    struct stat s;
    Volume *vol = volume_for_path("/wimax");
    if (restore_wimax && vol != NULL && 0 == stat(vol->device, &s))
    {
        char serialno[PROPERTY_VALUE_MAX];
        
        serialno[0] = 0;
        property_get("ro.serialno", serialno, "");
        sprintf(tmp, "%s/wimax.%s.img", backup_path, serialno);

        struct stat st;
        if (0 != stat(tmp, &st))
        {
            ui_print("WARNING: WiMAX partition exists, but nandroid\n");
            ui_print("         backup does not contain WiMAX image.\n");
            ui_print("         You should create a new backup to\n");
            ui_print("         protect your WiMAX keys.\n");
        }
        else
        {
            ui_print("Erasing WiMAX before restore...\n");
            if (0 != (ret = format_volume("/wimax")))
                return print_and_error("Error while formatting wimax!\n");
            ui_print("Restoring WiMAX image...\n");
            if (0 != (ret = restore_raw_partition(vol->fs_type, vol->device, tmp)))
                return ret;
        }
    }

    if (restore_system && 0 != (ret = nandroid_restore_partition(backup_path, "/system")))
        return ret;

    if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/data")))
        return ret;
        
    if (has_datadata()) {
        if (restore_data && 0 != (ret = nandroid_restore_partition(backup_path, "/datadata")))
            return ret;
    }

    if (restore_data && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/sdcard/.android_secure", 0)))
        return ret;

    if (restore_cache && 0 != (ret = nandroid_restore_partition_extended(backup_path, "/cache", 0)))
        return ret;

    if (restore_sdext && 0 != (ret = nandroid_restore_partition(backup_path, "/sd-ext")))
        return ret; 
   

    sync();
    ui_set_background(BACKGROUND_ICON_NONE);
    ui_reset_progress();
   // printf("Restore path is: '%s' \n", backup_path); //if will get the full backup path
    //sdcard/miui_recovery/backup/data/1130722-0718
    //
    ui_print("\nRestore complete!\n");
    return 0;
}

int nandroid_usage()
{
    printf("Usage: nandroid backup\n");
    printf("Usage: nandroid restore <directory>\n");
    return 1;
}

int nandroid_main(int argc, char** argv)
{

	char backup_path[PATH_MAX];
    if (argc > 3 || argc < 2)
        return nandroid_usage();
    
    if (strcmp("backup", argv[1]) == 0)
    {
        if (argc != 2)
            return nandroid_usage();
        
        nandroid_generate_timestamp_path(backup_path);
        return nandroid_backup(backup_path);
    }

    if (strcmp("restore", argv[1]) == 0)
    {
        if (argc != 3)
            return nandroid_usage();
        return nandroid_restore(argv[2], 1, 1, 1, 1, 1, 0);
    }
    
    return nandroid_usage();
}


