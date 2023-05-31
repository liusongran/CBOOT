/*
 * Copyright (c) 2018-2019, NVIDIA Corporation.  All Rights Reserved.
 *
 * NVIDIA Corporation and its licensors retain all intellectual property and
 * proprietary rights in and to this software and related documentation.  Any
 * use, reproduction, disclosure or distribution of this software and related
 * documentation without an express license agreement from NVIDIA Corporation
 * is strictly prohibited.
 */

#define MODULE TEGRABL_ERR_FILE_MANAGER

#include "build_config.h"
#include <string.h>
#include <tegrabl_utils.h>
#include <tegrabl_malloc.h>
#include <tegrabl_partition_manager.h>
#include <tegrabl_file_manager.h>
#include <tegrabl_debug.h>
#include <inttypes.h>
#include <tegrabl_error.h>
#include <fs.h>
#include <tegrabl_cbo.h>
#include <tegrabl_linuxboot_helper.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_COPY1_PATH				"/boot/copy1"
#define IMAGE_COPY2_PATH				"/boot/copy2"

static struct tegrabl_fm_handle *fm_handle;

static char *usb_prefix = "/usb";
static char *sdcard_prefix = "/sd";
static char *sdmmc_user_prefix = "/sdmmc_user";
static char *sdmmc_boot_prefix = "/sdmmc_boot";

static char *get_prefix(uint32_t device_id)
{
	char *prefix = NULL;
	uint32_t bdev_id = BITFIELD_GET(device_id, 16, 16);

	switch (bdev_id) {
	case TEGRABL_STORAGE_USB_MS:
		prefix = usb_prefix;
		break;
	case TEGRABL_STORAGE_SDCARD:
		prefix = sdcard_prefix;
		break;
	case TEGRABL_STORAGE_SDMMC_USER:
		prefix = sdmmc_user_prefix;
		break;
	case TEGRABL_STORAGE_SDMMC_BOOT:
		prefix = sdmmc_boot_prefix;
		break;
	default:
		; /* Do nothing */
		break;
	}

	return prefix;
}

/**
* @brief Get file manager handle
*
* @return File manager handle
*/
struct tegrabl_fm_handle *tegrabl_file_manager_get_handle(void)
{
	return fm_handle;
}

/**
* @brief Publish the partitions available in the GPT and try to mount the FS in "BOOT" partition.
* If GPT itself is not available, try to detect FS from sector 0x0 and mount it.
*
* @param bdev storage device pointer
*
* @return File manager handle
*/
tegrabl_error_t tegrabl_fm_publish(tegrabl_bdev_t *bdev, struct tegrabl_fm_handle **handle)
{
	tegrabl_error_t err = TEGRABL_NO_ERROR;
#if defined(CONFIG_ENABLE_EXTLINUX_BOOT)
	char *nvidia_boot_pt_guid = NULL;
	struct tegrabl_partition boot_partition = {0};
	char *fs_type;
	char *prefix = NULL;
	uint32_t detect_fs_sector;
	int32_t status = 0x0;
#endif

	pr_trace("%s(): %u\n", __func__, __LINE__);

	/* Allocate memory for handle */
	fm_handle = tegrabl_malloc(sizeof(struct tegrabl_fm_handle));
	if (fm_handle == NULL) {
		pr_error("Failed to allocate memory for fm handle!!\n");
		err = TEGRABL_ERROR(TEGRABL_ERR_NO_MEMORY, 0x0);
		goto fail;
	}
	memset(fm_handle, 0x0, sizeof(struct tegrabl_fm_handle));
	fm_handle->bdev = bdev;

	/* Publish the partitions available in the block device (no error means GPT exists) */
	err = tegrabl_partition_publish(bdev, 0);
	if (err != TEGRABL_NO_ERROR) {
#if defined(CONFIG_ENABLE_EXTLINUX_BOOT)
		/* GPT does not exist, detect FS from start sector of the device */
		detect_fs_sector = 0x0;
		goto detect_fs;
#else
		goto fail;
#endif
	}

#if defined(CONFIG_ENABLE_EXTLINUX_BOOT)
	pr_info("Look for boot partition\n");
	nvidia_boot_pt_guid = tegrabl_get_boot_pt_guid();
	err = tegrabl_partition_boot_guid_lookup_bdev(nvidia_boot_pt_guid, &boot_partition, bdev);
	if (err != TEGRABL_NO_ERROR) {
		goto fail;
	}
	/* BOOT partition exists! */
	detect_fs_sector = boot_partition.partition_info->start_sector;

detect_fs:
	pr_info("Detect filesystem\n");
	fs_type = fs_detect(bdev, detect_fs_sector);
	if (fs_type == NULL) {
		/* No supported FS detected */
		err = TEGRABL_ERROR(TEGRABL_ERR_INVALID, 0x0);
		goto fail;
	}

	prefix = get_prefix(bdev->device_id);
	if (prefix == NULL) {
		/* Unsupported device */
		err = TEGRABL_ERROR(TEGRABL_ERR_INVALID, 0x1);
		pr_error("Unsupported storage device for File system!!\n");
		goto fail;
	}

	/* Mount fs */
	status = fs_mount(prefix, fs_type, bdev, detect_fs_sector);
	if (status != 0x0) {
		/* Mount failed */
		err = TEGRABL_ERROR(TEGRABL_ERR_INVALID, 0x2);
		pr_error("Failed to mount file system!!\n");
		goto fail;
	}

	fm_handle->fs_type = fs_type;
	fm_handle->start_sector = detect_fs_sector;
	fm_handle->mount_path = prefix;
#endif  /* CONFIG_ENABLE_EXTLINUX_BOOT */

fail:
	if (handle) {
		*handle = fm_handle;
	}
	return err;
}

tegrabl_error_t tegrabl_fm_read_partition(struct tegrabl_bdev *bdev,
										  char *partition_name,
										  void *load_address,
										  uint32_t *size)
{
	struct tegrabl_partition partition;
	uint32_t partition_size;
	tegrabl_error_t err = TEGRABL_NO_ERROR;

	pr_trace("%s(): %u\n", __func__, __LINE__);

	if ((bdev == NULL) || (partition_name == NULL) || (load_address == NULL)) {
		pr_error("Invalid args passed (bdev: %p, pt name: %s, load addr: %p)\n",
				 bdev, partition_name, load_address);
		err = TEGRABL_ERROR(TEGRABL_ERR_INVALID, 0x3);
		goto fail;
	}

	/* Get partition info */
	err = tegrabl_partition_lookup_bdev(partition_name, &partition, bdev);
	if (err != TEGRABL_NO_ERROR) {
		pr_error("Cannot open partition %s\n", partition_name);
		TEGRABL_SET_HIGHEST_MODULE(err);
		goto fail;
	}

	/* Get partition size */
	partition_size = tegrabl_partition_size(&partition);
	pr_debug("Size of partition: %u\n", partition_size);
	if (!partition_size) {
		err = TEGRABL_ERROR(TEGRABL_ERR_INVALID, 0x4);
		goto fail;
	}

	/* Check if the partition load may over flow */
	if (size != NULL) {
		if (*size < partition_size) {
			pr_info("Insufficient buffer size\n");
			err = TEGRABL_ERROR(TEGRABL_ERR_OVERFLOW, 0x1);
			goto fail;
		}
	}

	/* Read the partition */
	err = tegrabl_partition_read(&partition, load_address, partition_size);
	if (err != TEGRABL_NO_ERROR) {
		pr_error("Error reading partition %s\n", partition_name);
		TEGRABL_SET_HIGHEST_MODULE(err);
		goto fail;
	}

	/* Return size */
	if (size) {
		*size = partition_size;
	}

fail:
	return err;
}

/**
* @brief Read the file from the filesystem if possible, otherwise read form the partiton.
*
* @param handle pointer to filemanager handle
* @param file_path file name along with the path
* @param partition_name partition to read from in case if file read fails from filesystem.
* @param load_address address into which the file/partition needs to be loaded.
* @param size max size of the file expected by the caller.
* @param is_file_loaded_from_fs specify whether file is loaded from filesystem or partition.
*
* @return TEGRABL_NO_ERROR if success, specific error if fails.
*/
tegrabl_error_t tegrabl_fm_read(struct tegrabl_fm_handle *handle,
								char *file_path,
								char *partition_name,
								void *load_address,
								uint32_t *size,
								bool *is_file_loaded_from_fs)
{
	tegrabl_error_t err = TEGRABL_NO_ERROR;
	char path[200];
	filehandle *fh = NULL;
	struct file_stat stat;
	int32_t status = 0x0;

	pr_trace("%s(): %u\n", __func__, __LINE__);

	if (is_file_loaded_from_fs != NULL) {
		*is_file_loaded_from_fs = false;
	}

	if (handle == NULL) {
		pr_error("Null handle passed\n");
		err = TEGRABL_ERROR(TEGRABL_ERR_INVALID, 0x3);
		goto fail;
	}

	if ((file_path == NULL) || (handle->mount_path == NULL)) {
		goto load_from_partition;
	}

	/* Load file from FS */
	if ((strlen(handle->mount_path) + strlen(file_path)) >= sizeof(path)) {
		pr_error("Destination buffer is insufficient to hold file path\n");
		err = TEGRABL_ERROR(TEGRABL_ERR_OVERFLOW, 0x2);
		goto load_from_partition;
	}
	memset(path, '\0', ARRAY_SIZE(path));
	strcpy(path, handle->mount_path);
	strcat(path, file_path);

	pr_info("rootfs path: %s\n", path);

	status = fs_open_file(path, &fh);
	if (status != 0x0) {
		pr_error("file %s open failed!!\n", path);
		err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x0);
		goto load_from_partition;
	}

	status = fs_stat_file(fh, &stat);
	if (status != 0x0) {
		pr_error("file %s stat failed!!\n", path);
		err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x1);
		goto load_from_partition;
	}

	/* Check for file overflow */
	if (*size < stat.size) {
		err = TEGRABL_ERROR(TEGRABL_ERR_OVERFLOW, 0x0);
		goto load_from_partition;
	}

	status = fs_read_file(fh, load_address, 0x0, stat.size);
	if (status < 0) {
		pr_error("file %s read failed!!\n", path);
		err = TEGRABL_ERROR(TEGRABL_ERR_READ_FAILED, 0x1);
		goto load_from_partition;
	}

	//假设只验证Image文件
	if(!strcmp(file_path,"/boot/Image")){
		pr_info("Compare original image file and backup image files\n");
		//尽量不分配新的空间，使用之前分配的空间
		//将Image文件的信息输出
		pr_info("Original image file ( Image ) size : %ld\n",stat.size);
		uint32_t a = tegrabl_utils_crc32(0,load_address,stat.size);

		//将copy1文件内容复制到内存中
		memset(path, '\0', ARRAY_SIZE(path));
		strcpy(path, handle->mount_path);
		strcat(path, IMAGE_COPY1_PATH);
		
		status = fs_open_file(path, &fh);
		if (status != 0x0) {
			pr_error("file %s open failed!!\n", path);
			err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x0);
			goto load_from_partition;
		}

		status = fs_stat_file(fh, &stat);
		if (status != 0x0) {
			pr_error("file %s stat failed!!\n", path);
			err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x1);
			goto load_from_partition;
		}

		if (*size < stat.size) {
			err = TEGRABL_ERROR(TEGRABL_ERR_OVERFLOW, 0x0);
			goto load_from_partition;
		}

		status = fs_read_file(fh, load_address, 0x0, stat.size);
		if (status < 0) {
			pr_error("file %s read failed!!\n", path);
			err = TEGRABL_ERROR(TEGRABL_ERR_READ_FAILED, 0x1);
			goto load_from_partition;
		}
		pr_info("Backup   image file ( copy1 ) size : %ld\n",stat.size);
		uint32_t b = tegrabl_utils_crc32(0,load_address,stat.size);
		//将copy2文件的内容复制到内存中
		memset(path, '\0', ARRAY_SIZE(path));
		strcpy(path, handle->mount_path);
		strcat(path, IMAGE_COPY2_PATH);

		status = fs_open_file(path, &fh);
		if (status != 0x0) {
			pr_error("file %s open failed!!\n", path);
			err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x0);
			goto load_from_partition;
		}

		status = fs_stat_file(fh, &stat);
		if (status != 0x0) {
			pr_error("file %s stat failed!!\n", path);
			err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x1);
			goto load_from_partition;
		}

		if (*size < stat.size) {
			err = TEGRABL_ERROR(TEGRABL_ERR_OVERFLOW, 0x0);
			goto load_from_partition;
		}

		status = fs_read_file(fh, load_address, 0x0, stat.size);
		if (status < 0) {
			pr_error("file %s read failed!!\n", path);
			err = TEGRABL_ERROR(TEGRABL_ERR_READ_FAILED, 0x1);
			goto load_from_partition;
		}

		uint32_t c = tegrabl_utils_crc32(0,load_address,stat.size);
		pr_info("Backup   image file ( copy2 ) size : %ld\n",stat.size);

		pr_info("Original image file ( Image ) CRC = %d\n",a);
		pr_info("Backup   image file ( copy1 ) CRC = %d\n",b);
		pr_info("Backup   image file ( copy2 ) CRC = %d\n",c);

		/*
			memset(path, '\0', ARRAY_SIZE(path));
			strcpy(path, handle->mount_path);
			strcat(path, file_path);

			status = fs_open_file(path, &fh);
			if (status != 0x0) {
				pr_error("file %s open failed!!\n", path);
				err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x0);
				goto load_from_partition;
			}

			status = fs_stat_file(fh, &stat);
			if (status != 0x0) {
				pr_error("file %s stat failed!!\n", path);
				err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x1);
				goto load_from_partition;
			}

			if (*size < stat.size) {
				err = TEGRABL_ERROR(TEGRABL_ERR_OVERFLOW, 0x0);
				goto load_from_partition;
			}

			status = fs_read_file(fh, load_address, 0x0, stat.size);
			if (status < 0) {
				pr_error("file %s read failed!!\n", path);
				err = TEGRABL_ERROR(TEGRABL_ERR_READ_FAILED, 0x1);
				goto load_from_partition;
			}
		*/

		if(a==b||a==c){//Image文件未出现错误
			pr_info("Original image file ( Image ) not affected!\n");
			pr_info("Boot with Original image file ( Image )!\n");

			//再复制一次Image文件
			memset(path, '\0', ARRAY_SIZE(path));
			strcpy(path, handle->mount_path);
			strcat(path, file_path);

			status = fs_open_file(path, &fh);
			if (status != 0x0) {
				pr_error("file %s open failed!!\n", path);
				err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x0);
				goto load_from_partition;
			}

			status = fs_stat_file(fh, &stat);
			if (status != 0x0) {
				pr_error("file %s stat failed!!\n", path);
				err = TEGRABL_ERROR(TEGRABL_ERR_OPEN_FAILED, 0x1);
				goto load_from_partition;
			}

			if (*size < stat.size) {
				err = TEGRABL_ERROR(TEGRABL_ERR_OVERFLOW, 0x0);
				goto load_from_partition;
			}

			status = fs_read_file(fh, load_address, 0x0, stat.size);
			if (status < 0) {
				pr_error("file %s read failed!!\n", path);
				err = TEGRABL_ERROR(TEGRABL_ERR_READ_FAILED, 0x1);
				goto load_from_partition;
			}

		}else if(a!=b&&a!=c&&b==c){//Image文件出现错误，copy1文件和copy2文件未出现错误
			pr_info("Original image file ( Image ) affected!\n");
			pr_info("Boot with Backup image file ( copy1 or copy2 )!\n");
		}else if(a!=b&&a!=c&&b!=c){//不能确定哪个文件出现错误
			pr_info("Original image file ( Image ) and backup image files ( copy1 or copy2 ) affected!\n");
			pr_info("Start bitwise comparison!\n");

			uint8_t *temp_buf2 = NULL;
			temp_buf2 = (uint8_t*)malloc(sizeof(uint8_t)*2000000);
			if(temp_buf2 == NULL)
				pr_info("temp_buff2 malloc failed!\n");
			uint8_t *temp_buf3 = NULL;
			temp_buf3 = (uint8_t*)malloc(sizeof(uint8_t)*2000000);
			if(temp_buf3 == NULL)
				pr_info("temp_buff3 malloc failed!\n");

			uint64_t start=0;
			uint32_t d = 0;
			uint64_t *a=NULL;
			a=(uint64_t *)malloc(sizeof(uint64_t)*1000000);
			if(a==NULL)pr_info("a malloc failed!\n");
			uint8_t *b=NULL;
			b=(uint8_t*)malloc(sizeof(uint8_t)*1000000);
			uint32_t size1=0;
			while(start < stat.size){
				memset(path, '\0', ARRAY_SIZE(path));
				strcpy(path, handle->mount_path);
				strcat(path, IMAGE_COPY1_PATH);

				status = fs_open_file(path, &fh);
				if (status != 0x0) {
					pr_error("file %s open failed!!\n", path);
				}

				status = fs_read_file(fh, load_address, 0x0, stat.size);
				if (status < 0) {
					pr_error("file %s read failed!!\n", path);
				}

				for(uint64_t i=0;i<2000000&&(start+i)<stat.size;++i){
					temp_buf2[i]=*(uint8_t*)(load_address+start+i);
				}

				memset(path, '\0', ARRAY_SIZE(path));
				strcpy(path, handle->mount_path);
				strcat(path, IMAGE_COPY2_PATH);

				status = fs_open_file(path, &fh);
				if (status != 0x0) {
					pr_error("file %s open failed!!\n", path);
				}

				status = fs_read_file(fh, load_address, 0x0, stat.size);
				if (status < 0) {
					pr_error("file %s read failed!!\n", path);
				}

				for(uint64_t i=0;i<2000000&&(start+i)<stat.size;++i){
					temp_buf3[i]=*(uint8_t*)(load_address+start+i);
				}

				memset(path, '\0', ARRAY_SIZE(path));
				strcpy(path, handle->mount_path);
				strcat(path, file_path);

				status = fs_open_file(path, &fh);
				if (status != 0x0) {
					pr_error("file %s open failed!!\n", path);
				}

				status = fs_read_file(fh, load_address, 0x0, stat.size);
				if (status < 0) {
					pr_error("file %s read failed!!\n", path);
				}

				for(uint64_t i=0;i<2000000&&(start+i)<stat.size;++i){
					uint8_t t=*(uint8_t*)(load_address+start+i);
					if(t!=temp_buf2[i]&&temp_buf2[i]==temp_buf3[i]){
						//memcpy((load_address+start+i),(void *)(temp_buf2+i),sizeof(uint8_t));
						a[size1]=start+i;
						b[size1++]=temp_buf2[i];
						pr_info("Flipping is found and handled!\n");
					}
					else if(t!=temp_buf2[i]&&temp_buf2[i]!=temp_buf3[i]&&t!=temp_buf3[i]){
						uint8_t out1[8],out2[8],out3[8];
						unsigned int mask = 1U << (8-1);
						for (int j = 0; j < 8; j++) {
							out1[j] = (t & mask) ? 1 : 0;
							t <<= 1;
						}
						for (int j = 0; j < 8; j++) {
							out2[j] = (temp_buf2[i] & mask) ? 1 : 0;
							temp_buf2[i] <<= 1;
						}
						for (int j = 0; j < 8; j++) {
							out3[j] = (temp_buf3[i] & mask) ? 1 : 0;
							temp_buf3[i] <<= 1;
						}
						for (int j = 0; j < 8; j++) {
							if(out2[j]==out3[j]&&out1[j]!=out2[j]){
								out1[j]=out2[j];
							}
						}
						t=0;
						for (int j = 8;j > 0; --j){
							if(out1[j] == 1){
								uint8_t s=1;
								for(int k=1;k<8-j;++k)
									s *= 2;
								t+=s;
							}
						}
						//memcpy((load_address+start+i),&t,sizeof(uint8_t));
						a[size1]=start+i;
						b[size1++]=t;
						pr_info("Flipping is found and handled!\n");
					}
				}

				if(start+2000000>stat.size){
					//d += tegrabl_utils_crc32(0,load_address+start,stat.size-start);
					start=stat.size;
				}
				else{
					//d += tegrabl_utils_crc32(0,load_address+start,2000000);
					start+=2000000;
				}
			}
			for(uint32_t i=0;i<size1;++i){
				memcpy((load_address+a[i]),b+i,sizeof(uint8_t));
			}
			pr_info("All bits are compared!\n");
			d = tegrabl_utils_crc32(0,load_address,stat.size);
			pr_info("Newly formed image file CRC = %d\n",d);
		}
	}

	*size = stat.size;
	if (is_file_loaded_from_fs) {
		*is_file_loaded_from_fs = true;
	}

	/* Save the handle */
	fm_handle = handle;

	goto fail;

load_from_partition:
	if (partition_name == NULL) {
		goto fail;
	}

	pr_info("Fallback: Loading from %s partition of %s device ...\n",
			partition_name,
			tegrabl_blockdev_get_name(tegrabl_blockdev_get_storage_type(handle->bdev)));
	err = tegrabl_fm_read_partition(handle->bdev, partition_name, load_address, size);
	if (err != TEGRABL_NO_ERROR) {
		goto fail;
	}

fail:

	if (fh != NULL) {
		fs_close_file(fh);
	}
	return err;
}

/**
* @brief Unmount the filesystem and freeup memory.
*
* @param handle filemanager handle to unmount and free the space.
*/
void tegrabl_fm_close(struct tegrabl_fm_handle *handle)
{
	if (handle == NULL) {
		goto fail;
	}

	if (handle->mount_path != NULL) {
		fs_unmount(handle->mount_path);
	}

	tegrabl_free(handle);
	fm_handle = NULL;

fail:
	return;
}
