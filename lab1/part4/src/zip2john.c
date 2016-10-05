/* zip2john processes input ZIP files into a format suitable for use with JtR.
 *
 * This software is Copyright (c) 2011, Dhiru Kholia <dhiru.kholia at gmail.com>,
 * and it is hereby released to the general public under the following terms:
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted.
 *
 * Updated in Aug 2011 by JimF.  Added PKZIP 'old' encryption.  The signature on the
 * pkzip will be $pkzip$ and does not look like the AES version written by Dhiru
 * Also fixed some porting issues, such as variables needing declared at top of blocks.
 *
 * References:
 *
 * 1. http://www.winzip.com/aes_info.htm
 * 2. http://www.winzip.com/aes_tips.htm
 * 4. ftp://ftp.info-zip.org/pub/infozip/doc/appnote-iz-latest.zip
 * 5. Nathan Moinvaziri's work in extending minizip to support AES.
 * 6. http://oldhome.schmorp.de/marc/fcrackzip.html (coding hints)
 * 7. http://www.pkware.com/documents/casestudies/APPNOTE.TXT
 * 8. http://gladman.plushost.co.uk/oldsite/cryptography_technology/fileencrypt/index.php
 *   (borrowed files have "gladman_" prepended to them)
 * 9. http://svn.assembla.com/svn/os2utils/unzip60f/proginfo/extrafld.txt
 * 10. http://emerge.hg.sourceforge.net/hgweb/emerge/emerge/diff/c2f208617d32/Source/unzip/proginfo/extrafld.txt
 *
 * Usage:
 *
 * 1. Run zip2john on zip file(s) as "zip2john [zip files]".
 *    Output is written to standard output.
 * 2. Run JtR on the output generated by zip2john as "john [output file]".
 *
 * Output Line Format:
 *
 * For type = 0, for ZIP files encrypted using AES
 * filename:$zip$*type*hex(CRC)*encryption_strength*hex(salt)*hex(password_verfication_value):hex(authentication_code)
 *
 * For original pkzip encryption:  (JimF, with longer explaination of fields)
 * filename:$pkzip$C*B*[DT*MT{CL*UL*CR*OF*OX}*CT*DL*CS*DA]*$/pkzip$   (deprecated)
 * filename:$pkzip2$C*B*[DT*MT{CL*UL*CR*OF*OX}*CT*DL*CS*TC*DA]*$/pkzip2$   (new format, with 2 checksums)
 * All numeric and 'binary data' fields are stored in hex.
 *
 * C   is the count of hashes present (the array of items, inside the []  C can be 1 to 3.).
 * B   is number of valid bytes in the checksum (1 or 2).  Unix zip is 2 bytes, all others are 1 (NOTE, some can be 0)
 * ARRAY of data starts here
 *   DT  is a "Data Type enum".  This will be 1 2 or 3.  1 is 'partial'. 2 and 3 are full file data (2 is inline, 3 is load from file).
 *   MT  Magic Type enum.  0 is no 'type'.  255 is 'text'. Other types (like MS Doc, GIF, etc), see source.
 *     NOTE, CL, DL, CRC, OFF are only present if DT != 1
 *     CL  Compressed length of file blob data (includes 12 byte IV).
 *     UL  Uncompressed length of the file.
 *     CR  CRC32 of the 'final' file.
 *     OF  Offset to the PK\x3\x4 record for this file data. If DT==2, then this will be a 0, as it is not needed, all of the data is already included in the line.
 *     OX  Additional offset (past OF), to get to the zip data within the file.
 *     END OF 'optional' fields.
 *   CT  Compression type  (0 or 8)  0 is stored, 8 is imploded.
 *   DL  Length of the DA data.
 *   CS  2 bytes of checksum data.
 *   TC  2 bytes of checksun data (fron timestamp)
 *   DA  This is the 'data'.  It will be hex data if DT==1 or 2. If DT==3, then it is a filename (name of the .zip file).
 * END of array item.  There will be C (count) array items.
 * The format string will end with $/pkzip$
 *
 * The AES-zip format redone by JimF, Summer 2014.  Spent some time to understand the AES authentication code,
 * and now have placed code to do this. However, this required format change.  The old AES format was:
 *
 *    For type = 0, for ZIP files encrypted using AES
 *    filename:$zip$*type*hex(CRC)*encryption_strength*hex(salt)*hex(password_verfication_value):hex(authentication_code)
 *     NOTE, the authentication code was NOT part of this, even though documented in this file. nor is hex(CRC) a part.
 *
 * The new format is:  (and the $zip$ is deprecated)
 *
 *    filename:$zip2$*Ty*Mo*Ma*Sa*Va*Le*DF*Au*$/zip2$
 *    Ty = type (0) and ignored.
 *    Mo = mode (1 2 3 for 128/192/256 bit)
 *    Ma = magic (file magic).  This is reservered for now.  See pkzip_fmt_plug.c or zip2john.c for information.
 *         For now, this must be a '0'
 *    Sa = salt(hex).   8, 12 or 16 bytes of salt (depends on mode)
 *    Va = Verification bytes(hex) (2 byte quick checker)
 *    Le = real compr len (hex) length of compressed/encrypted data (field DF)
 *    DF = compressed data DF can be Le*2 hex bytes, and if so, then it is the ENTIRE file blob written 'inline'.
 *         However, if the data blob is too long, then a .zip ZIPDATA_FILE_PTR_RECORD structure will be the 'contents' of DF
 *    Au = Authentication code (hex) a 10 byte hex value that is the hmac-sha1 of data over DF. This is the binary() value
 *
 *  ZIPDATA_FILE_PTR_RECORD  (this can be the 'DF' of this above hash line).
 *      *ZFILE*Fn*Oh*Ob*  (Note, the leading and trailing * are the * that 'wrap' the DF object.
 *  ZFILE This is the literal string ZFILE
 *  Fn    This is the name of the .zip file.  NOTE the user will need to keep the .zip file in proper locations (same as
 *        was seen when running zip2john. If the file is removed, this hash line will no longer be valid.
 *  Oh    Offset to the zip central header record for this blob.
 *  Ob    Offset to the start of the blob data
 */

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#if  (!AC_BUILT || HAVE_UNISTD_H) && !_MSC_VER
#include <unistd.h>
#endif

#include "common.h"
#include "jumbo.h"
#include "formats.h"
#include "stdint.h"
#include "pkzip.h"
#ifdef _MSC_VER
#include "missing_getopt.h"
#endif
#include "memdbg.h"

#define LARGE_ENOUGH 8192

static int checksum_only=0, use_magic=1;
static int force_2_byte_checksum = 0;
static char *ascii_fname, *only_fname;
static int inline_thr = MAX_INLINE_SIZE;
#define MAX_THR (LINE_BUFFER_SIZE / 2 - 2 * PLAINTEXT_BUFFER_SIZE)

static char *MagicTypes[]= { "", "DOC", "XLS", "DOT", "XLT", "EXE", "DLL", "ZIP", "BMP", "DIB", "GIF", "PDF", "GZ", "TGZ", "BZ2", "TZ2", "FLV", "SWF", "MP3", NULL };
static int  MagicToEnum[] = {0,  1,    1,     1,     1,     2,     2,     3,     4,     4,     5,     6,     7,    7,     8,     8,     9,     10,    11,  0};

static void process_old_zip(const char *fname);
static void process_file(const char *fname)
{
	unsigned char filename[1024];
	FILE *fp;
	int i;
	long long off_sig;
	char path[LARGE_ENOUGH];
	char *cur=0, *cp;
	uint32_t best_len = 0xffffffff;


	if (!(fp = fopen(fname, "rb"))) {
		fprintf(stderr, "! %s : %s\n", fname, strerror(errno));
		return;
	}

	while (!feof(fp)) {
		uint32_t id = fget32LE(fp);
		uint32_t store = 0;
		off_sig = (long long) (ftell(fp)-4);

		if (id == 0x04034b50UL) {	/* local header */
			uint16_t version = fget16LE(fp);
			uint16_t flags = fget16LE(fp);
			uint16_t compression_method = fget16LE(fp);
			uint16_t lastmod_time = fget16LE(fp);
			uint16_t lastmod_date = fget16LE(fp);
			uint32_t crc = fget32LE(fp);
			uint32_t compressed_size = fget32LE(fp);
			uint32_t uncompressed_size = fget32LE(fp);
			uint16_t filename_length = fget16LE(fp);
			uint16_t extrafield_length = fget16LE(fp);
			/* unused variables */
			(void) version;
			(void) lastmod_time;
			(void) lastmod_date;
			(void) crc;
			(void) uncompressed_size;

			if (filename_length > 250) {
				fprintf(stderr, "! %s: Invalid zip file, filename length too long!\n", fname);
				return;
			}
			if (fread(filename, 1, filename_length, fp) != filename_length) {
				fprintf(stderr, "Error, in fread of file data!\n");
				goto cleanup;
			}
			filename[filename_length] = 0;

			if (compression_method == 99) {	/* AES encryption */
				uint32_t real_cmpr_len;
				uint16_t efh_id = fget16LE(fp);
				uint16_t efh_datasize = fget16LE(fp);
				uint16_t efh_vendor_version = fget16LE(fp);
				uint16_t efh_vendor_id = fget16LE(fp);
				char efh_aes_strength = fgetc(fp);
				uint16_t actual_compression_method = fget16LE(fp);
				unsigned char salt[16], d;
				char *bname;
				int magic_enum = 0;  // reserved at 0 for now, we are not computing this (yet).

				strnzcpy(path, fname, sizeof(path));
				bname = basename(path);
				cp = cur;
				if ((unsigned)best_len < compressed_size) {
#if DEBUG
					printf ("This buffer not used, it is not 'best' size\n");
#endif
				} else {
					store = 1;
					best_len = compressed_size;
					if (cur) MEM_FREE(cur);
					cur = mem_alloc(compressed_size*2 + 400);
					cp = cur;
				}

#if DEBUG
				fprintf(stderr,
				    "%s->%s is using AES encryption, extrafield_length is %d\n",
				    fname, filename, extrafield_length);
#endif
				/* unused variables */
				(void) efh_id;
				(void) efh_datasize;
				(void) efh_vendor_version;
				(void) efh_vendor_id;
				(void) actual_compression_method; /* we need this!! */

				if (store) cp += sprintf(cp, "%s:$zip2$*0*%x*%x*", bname, efh_aes_strength, magic_enum);
				if (fread(salt, 1, 4+4*efh_aes_strength, fp) != 4+4*efh_aes_strength) {
						fprintf(stderr, "Error, in fread of file data!\n");
						goto cleanup;
				}

				for (i = 0; i < 4+4*efh_aes_strength; i++) {
					if (store) cp += sprintf(cp, "%c%c",
						itoa16[ARCH_INDEX(salt[i] >> 4)],
						itoa16[ARCH_INDEX(salt[i] & 0x0f)]);
				}
				if (store) cp += sprintf (cp, "*");
				// since in the format we read/compare this one, we do it char by
				// char, so there is no endianity swapping needed. (validator)
				for (i = 0; i < 2; i++) {
					d = fgetc(fp);
					if (store) cp += sprintf(cp, "%c%c",
						itoa16[ARCH_INDEX(d >> 4)],
						itoa16[ARCH_INDEX(d & 0x0f)]);
				}
				real_cmpr_len = compressed_size-2-(4+4*efh_aes_strength)-extrafield_length;
				// not quite sure why the real_cmpr_len is 'off by 1' ????
				++real_cmpr_len;
				if (store) cp += sprintf(cp, "*%x*", real_cmpr_len);
				if (real_cmpr_len < inline_thr) {
					for (i = 0; i < real_cmpr_len; i++) {
						d = fgetc(fp);
						if (store) cp += sprintf(cp, "%c%c",
							itoa16[ARCH_INDEX(d >> 4)],
							itoa16[ARCH_INDEX(d & 0x0f)]);
					}
				} else {
					if (store) cp += sprintf(cp, "ZFILE*%s*"LLx"*"LLx,
							fname, off_sig, (long long)(ftell(fp)));
					fseek(fp, real_cmpr_len, SEEK_CUR);
				}
				if (store) cp += sprintf(cp, "*");
				for (i = 0; i < 10; i++) {
					d = fgetc(fp);
					if (store) cp += sprintf(cp, "%c%c",
					    itoa16[ARCH_INDEX(d >> 4)],
					    itoa16[ARCH_INDEX(d & 0x0f)]);
				}
				for (d = ' '+1; d < '~'; ++d) {
					if (!strchr(fname, d) && d != ':' && !isxdigit(d))
						break;
				}
				if (store) cp += sprintf(cp, "*$/zip2$:::::%s\n", bname);
			} else if (flags & 1) {	/* old encryption */
				fclose(fp);
				fp = 0;
				process_old_zip(fname);
				return;
			} else {
				fprintf(stderr, "%s->%s is not encrypted!\n", fname,
				    filename);
				fseek(fp, extrafield_length, SEEK_CUR);
				fseek(fp, compressed_size, SEEK_CUR);
			}
		} else if (id == 0x08074b50UL) {	/* data descriptor */
			fseek(fp, 12, SEEK_CUR);
		} else if (id == 0x02014b50UL || id == 0x06054b50UL) {	/* central directory structures */
			goto cleanup;
		}
	}

cleanup:
	if (cur)
		printf ("%s\n",cur);
	fclose(fp);
}

/* instead of using anything from the process_file, we simply detected a encrypted old style
 * password, close the file, and call this function.  This function handles the older pkzip
 * password, while the process_file handles ONLY the AES from WinZip
 */
typedef struct _zip_ptr
{
	uint16_t      magic_type, cmptype;
	uint32_t      offset, offex, crc, cmp_len, decomp_len;
	char          chksum[5];
	char          chksum2[5];
	char         *hash_data;
} zip_ptr;
typedef struct _zip_file
{
	int unix_made;
	int check_in_crc;
	int check_bytes;
} zip_file;

static int magic_type(const char *filename) {
	char *Buf = str_alloc_copy((char*)filename), *cp;
	int i;

	if (!use_magic) return 0;

	strupr(Buf);
	if (ascii_fname && !strcasecmp(Buf, ascii_fname))
		return 255;

	cp = strrchr(Buf, '.');
	if (!cp) return 0;
	++cp;
	for (i = 1; MagicTypes[i]; ++i)
		if (!strcmp(cp, MagicTypes[i]))
			return MagicToEnum[i];
	return 0;
}
static char *toHex(unsigned char *p, int len) {
	static char Buf[4096];
	char *cp = Buf;
	int i;
	for (i = 0; i < len; ++i)
		cp += sprintf(cp, "%02x", p[i]);
	return Buf;
}
static int LoadZipBlob(FILE *fp, zip_ptr *p, zip_file *zfp, const char *zip_fname)
{
	uint16_t version,flags,lastmod_time,lastmod_date,filename_length,extrafield_length;
	unsigned char filename[1024];

	memset(p, 0, sizeof(*p));

	p->offset = ftell(fp)-4;
	version = fget16LE(fp);
	flags = fget16LE(fp);
	p->cmptype = fget16LE(fp);
	lastmod_time = fget16LE(fp);
	lastmod_date = fget16LE(fp);
	p->crc = fget32LE(fp);
	p->cmp_len= fget32LE(fp);
	p->decomp_len = fget32LE(fp);
	filename_length = fget16LE(fp);
	extrafield_length = fget16LE(fp);
	/* unused variables */
	(void) lastmod_date;

	if (fread(filename, 1, filename_length, fp) != filename_length) {
		fprintf(stderr, "Error, fread could not read the data from the file:  %s\n", zip_fname);
		return 0;
	}
	filename[filename_length] = 0;
	p->magic_type = magic_type((char*)filename);

	p->offex = 30 + filename_length + extrafield_length;

	// we only handle implode or store.
	// 0x314 was seen at 2012 CMIYC ?? I have to look into that one.
	fprintf(stderr, "ver %0x  ", version);
	if ( !(only_fname && strcmp(only_fname, (char*)filename)) && (version == 0x14||version==0xA||version == 0x314) && (flags & 1)) {
		uint16_t extra_len_used = 0;
		if (flags & 8) {
			while (extra_len_used < extrafield_length) {
				uint16_t efh_id = fget16LE(fp);
				uint16_t efh_datasize = fget16LE(fp);
				extra_len_used += 4 + efh_datasize;
				fseek(fp, efh_datasize, SEEK_CUR);
				fprintf(stderr, "efh %04x  ", efh_id);
				//http://svn.assembla.com/svn/os2utils/unzip60f/proginfo/extrafld.txt
				//http://emerge.hg.sourceforge.net/hgweb/emerge/emerge/diff/c2f208617d32/Source/unzip/proginfo/extrafld.txt
				if (efh_id == 0x07c8 ||  // Info-ZIP Macintosh (old, J. Lee)
					efh_id == 0x334d ||  // Info-ZIP Macintosh (new, D. Haase's 'Mac3' field)
					efh_id == 0x4d49 ||  // Info-ZIP OpenVMS (obsolete)
					efh_id == 0x5855 ||  // Info-ZIP UNIX (original; also OS/2, NT, etc.)
					efh_id == 0x6375 ||  // Info-ZIP UTF-8 comment field
					efh_id == 0x7075 ||  // Info-ZIP UTF-8 name field
					efh_id == 0x7855 ||  // Info-ZIP UNIX (16-bit UID/GID info)
					efh_id == 0x7875)    // Info-ZIP UNIX 3rd generation (generic UID/GID, ...)

					// 7zip ALSO is 2 byte checksum, but I have no way to find them.  NOTE, it is 2 bytes of CRC, not timestamp like InfoZip.
					// OLD winzip (I think 8.01 or before), is also supposed to be 2 byte.
					// old v1 pkzip (the DOS builds) are 2 byte checksums.
				{
					zfp->unix_made = 1;
					zfp->check_bytes = 2;
					zfp->check_in_crc = 0;
				}
			}
		}
		else if (extrafield_length)
			fseek(fp, extrafield_length, SEEK_CUR);

		if (force_2_byte_checksum)
			zfp->check_bytes = 2;

		fprintf(stderr,
			"%s->%s PKZIP Encr:%s%s cmplen=%d, decmplen=%d, crc=%X\n",
			jtr_basename(zip_fname), filename, zfp->check_bytes==2?" 2b chk,":"", zfp->check_in_crc?"":" TS_chk,", p->cmp_len, p->decomp_len, p->crc);

		p->hash_data = mem_alloc_tiny(p->cmp_len+1, MEM_ALIGN_WORD);
		if (fread(p->hash_data, 1, p->cmp_len, fp) != p->cmp_len) {
			fprintf(stderr, "Error, fread could not read the data from the file:  %s\n", zip_fname);
			return 0;
		}

		// Ok, now set checksum bytes.  This will depend upon if from crc, or from timestamp
			sprintf (p->chksum, "%02x%02x", (p->crc>>24)&0xFF, (p->crc>>16)&0xFF);
		sprintf (p->chksum2, "%02x%02x", lastmod_time>>8, lastmod_time&0xFF);

		return 1;

	}
	fprintf(stderr, "%s->%s is not encrypted, or stored with non-handled compression type\n", zip_fname, filename);
	fseek(fp, extrafield_length, SEEK_CUR);
	fseek(fp, p->cmp_len, SEEK_CUR);

	return 0;
}

static void process_old_zip(const char *fname)
{
	FILE *fp;

	int count_of_hashes = 0;
	zip_ptr hashes[3], curzip;
	zip_file zfp;

	char path[LARGE_ENOUGH];

	zfp.check_in_crc = 1;
	zfp.check_bytes = 1;
	zfp.unix_made = 0;

	if (!(fp = fopen(fname, "rb"))) {
		fprintf(stderr, "! %s : %s\n", fname, strerror(errno));
		return;
	}

	while (!feof(fp)) {
		uint32_t id = fget32LE(fp);

		if (id == 0x04034b50UL) {	/* local header */
			if (LoadZipBlob(fp, &curzip, &zfp, fname) && curzip.decomp_len > 3) {
				if (!count_of_hashes)
					memcpy(&(hashes[count_of_hashes++]), &curzip, sizeof(curzip));
				else {
					if (count_of_hashes == 1) {
						if (curzip.cmp_len < hashes[0].cmp_len) {
							memcpy(&(hashes[count_of_hashes++]), &(hashes[0]), sizeof(curzip));
							memcpy(&(hashes[0]), &curzip, sizeof(curzip));
						} else
							memcpy(&(hashes[count_of_hashes++]), &curzip, sizeof(curzip));
					}
					else if (count_of_hashes == 2) {
						if (curzip.cmp_len < hashes[0].cmp_len) {
							memcpy(&(hashes[count_of_hashes++]), &(hashes[1]), sizeof(curzip));
							memcpy(&(hashes[1]), &(hashes[0]), sizeof(curzip));
							memcpy(&(hashes[0]), &curzip, sizeof(curzip));
						} else if (curzip.cmp_len < hashes[1].cmp_len) {
							memcpy(&(hashes[count_of_hashes++]), &(hashes[1]), sizeof(curzip));
							memcpy(&(hashes[1]), &curzip, sizeof(curzip));
						} else
							memcpy(&(hashes[count_of_hashes++]), &curzip, sizeof(curzip));
					}
					else {
						int done=0;
						if (curzip.magic_type && curzip.cmp_len > hashes[0].cmp_len) {
							// if we have a magic type, we will replace any NON magic type, for the 2nd and 3rd largest, without caring about
							// the size.
							if (hashes[1].magic_type == 0) {
								if (hashes[2].cmp_len < curzip.cmp_len) {
									memcpy(&(hashes[1]), &(hashes[2]), sizeof(curzip));
									memcpy(&(hashes[2]), &curzip, sizeof(curzip));
									done=1;
								} else {
									memcpy(&(hashes[1]), &curzip, sizeof(curzip));
									done=1;
								}
							} else if (hashes[2].magic_type == 0) {
								if (hashes[1].cmp_len < curzip.cmp_len) {
									memcpy(&(hashes[2]), &curzip, sizeof(curzip));
									done=1;
								} else {
									memcpy(&(hashes[2]), &(hashes[1]), sizeof(curzip));
									memcpy(&(hashes[1]), &curzip, sizeof(curzip));
									done=1;
								}
							}
						}
						if (!done && curzip.cmp_len < hashes[0].cmp_len) {
							// we 'only' replace the smallest zip, and always keep as many any other magic as possible.
							if (hashes[0].magic_type == 0)
								memcpy(&(hashes[0]), &curzip, sizeof(curzip));
							else {
								// Ok, the 1st is a magic, we WILL keep it.
								if (hashes[1].magic_type) {  // Ok, we found our 2
									memcpy(&(hashes[2]), &(hashes[1]), sizeof(curzip));
									memcpy(&(hashes[1]), &(hashes[0]), sizeof(curzip));
									memcpy(&(hashes[0]), &curzip, sizeof(curzip));
								} else if (hashes[2].magic_type) {  // Ok, we found our 2
									memcpy(&(hashes[1]), &(hashes[0]), sizeof(curzip));
									memcpy(&(hashes[0]), &curzip, sizeof(curzip));
								} else {
									// found none.  So we will simply roll them down (like when #1 was a magic also).
									memcpy(&(hashes[2]), &(hashes[1]), sizeof(curzip));
									memcpy(&(hashes[1]), &(hashes[0]), sizeof(curzip));
									memcpy(&(hashes[0]), &curzip, sizeof(curzip));
								}
							}
						}
					}
				}
			}
		} else if (id == 0x08074b50UL) {	/* data descriptor */
			fseek(fp, 12, SEEK_CUR);
		} else if (id == 0x02014b50UL || id == 0x06054b50UL) {	/* central directory structures */
			goto print_and_cleanup;
		}
	}

print_and_cleanup:;
	if (count_of_hashes) {
		int i=1;
		char *bname;
		strnzcpy(path, fname, sizeof(path));
		bname = basename(path);

		printf ("%s:$pkzip2$%x*%x*", bname, count_of_hashes, zfp.check_bytes);
		if (checksum_only)
			i = 0;
		for (; i < count_of_hashes; ++i) {
			int len = 12+24;
			if (hashes[i].magic_type)
				len = 12+180;
			if (len > hashes[i].cmp_len)
				len = hashes[i].cmp_len; // even though we 'could' output a '2', we do not.  We only need one full inflate CRC check file.
			printf("1*%x*%x*%x*%s*%s*%s*", hashes[i].magic_type, hashes[i].cmptype, len, hashes[i].chksum, hashes[i].chksum2, toHex((unsigned char*)hashes[i].hash_data, len));
		}
		// Ok, now output the 'little' one (the first).
		if (!checksum_only) {
			printf("%x*%x*%x*%x*%x*%x*%x*%x*", hashes[0].cmp_len < inline_thr ? 2 : 3, hashes[0].magic_type, hashes[0].cmp_len, hashes[0].decomp_len, hashes[0].crc, hashes[0].offset, hashes[0].offex, hashes[0].cmptype);
			if (hashes[0].cmp_len < inline_thr)
				printf("%x*%s*%s*%s*", hashes[0].cmp_len, hashes[0].chksum, hashes[0].chksum2, toHex((unsigned char*)hashes[0].hash_data, hashes[0].cmp_len));
			else
				printf("%x*%s*%s*%s*", (unsigned int)strlen(fname), hashes[0].chksum, hashes[0].chksum2, fname);
		}
		printf("$/pkzip2$:::::%s\n", fname);
	}
	fclose(fp);
}

static int usage(char *name) {
	fprintf(stderr, "Usage: %s [options] [zip files]\n", name);
	fprintf(stderr, " -i <inline threshold> Set threshold for inlining data. Default is %d bytes\n", MAX_INLINE_SIZE);
	fprintf(stderr, "Options for 'old' PKZIP encrypted files only:\n");
	fprintf(stderr, " -a <filename>   This is a 'known' ASCII file\n");
	fprintf(stderr, "    Using 'ascii' mode is a serious speedup, IF all files are larger, and\n");
	fprintf(stderr, "    you KNOW that at least one of them starts out as 'pure' ASCII data\n");
	fprintf(stderr, " -o <filename>   Only use this file from the .zip file\n");
	fprintf(stderr, " -c This will create a 'checksum only' hash.  If there are many encrypted\n");
	fprintf(stderr, "    files in the .zip file, then this may be an option, and there will be\n");
	fprintf(stderr, "    enough data that false possitives will not be seen.  If the .zip is 2\n");
	fprintf(stderr, "    byte checksums, and there are 3 or more of them, then we have 48 bits\n");
	fprintf(stderr, "    knowledge, which 'may' be enough to crack the password, without having\n");
	fprintf(stderr, "    to force the user to have the .zip file present\n");
	fprintf(stderr, " -n Do not look for any magic file types in this zip.  If you know that\n");
	fprintf(stderr, "    are files with one of the 'magic' extensions, but they are not the right\n");
	fprintf(stderr, "    type files (some *.doc files that ARE NOT MS Office Word documents), then\n");
	fprintf(stderr, "    this switch will keep them from being detected this way.  NOTE, that\n");
	fprintf(stderr, "    the 'magic' logic will only be used in john, under certain situations.\n");
	fprintf(stderr, "    Most of these situations are when there are only 'stored' files in the zip\n");
	fprintf(stderr, " -2 Force 2 byte checksum computation\n");

	return EXIT_FAILURE;
}

int zip2john(int argc, char **argv)
{
	int c;

	/* Parse command line */
	while ((c = getopt(argc, argv, "a:o:i:cn2")) != -1) {
		switch (c) {
		case 'i':
			inline_thr = (int)strtol(optarg, NULL, 0);
			if (inline_thr > MAX_THR) {
				fprintf(stderr, "%s error: threshold %d, can't"
				        " be larger than %d\n", argv[0],
				        inline_thr, MAX_THR);
				return EXIT_FAILURE;
			}
			break;
		case 'a':
			ascii_fname = optarg;
			fprintf(stderr, "Using file %s as an 'ASCII' quick check file\n", ascii_fname);
			break;
		case 'o':
			only_fname = optarg;
			fprintf(stderr, "Using file %s as only file to check\n", only_fname);
			break;
		case 'c':
			checksum_only = 1;
			fprintf(stderr, "Outputing hashes that are 'checksum ONLY' hashes\n");
			break;
		case 'n':
			use_magic = 0;
			fprintf(stderr, "Ignoring any checking of file 'magic' signatures\n");
			break;
		case '2':
			force_2_byte_checksum = 1;
			fprintf(stderr, "Forcing a 2 byte checksum detection\n");
			break;
		case '?':
		default:
			return usage(argv[0]);
		}
	}
	argc -= optind;
	if(argc == 0)
		return usage(argv[0]);
	argv += optind;

	while(argc--)
		process_file(*argv++);

	cleanup_tiny_memory();
	MEMDBG_PROGRAM_EXIT_CHECKS(stderr);

	return EXIT_SUCCESS;
}
