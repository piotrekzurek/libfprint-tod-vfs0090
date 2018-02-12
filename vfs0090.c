/*
 * Validity VFS0090 driver for libfprint
 * Copyright (C) 2017 Nikita Mikhailov <nikita.s.mikhailov@gmail.com>
 * Copyright (C) 2018 Marco Trevisan <marco@ubuntu.com>
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define FP_COMPONENT "vfs0090"

#include <fp_internal.h>

#include <assembling.h>
#include <errno.h>
#include <nss.h>
#include <pk11pub.h>
#include <sechash.h>
#include <openssl/err.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <string.h>

#include "driver_ids.h"

#include "vfs0090.h"

/* The main driver structure */
struct vfs_dev_t {
	unsigned char *main_seed;
	unsigned int main_seed_length;

	unsigned char pubkey[VFS_PUBLIC_KEY_SIZE];
	unsigned char ecdsa_private_key[VFS_ECDSA_PRIVATE_KEY_SIZE];
	unsigned char masterkey_aes[VFS_MASTER_KEY_SIZE];
	unsigned char tls_certificate[G_N_ELEMENTS(TLS_CERTIFICATE_BASE)];

	/* Buffer for saving usb data through states */
	unsigned char *buffer;
	unsigned int buffer_length;

	unsigned char key_block[0x120];

	/* Current async transfer */
	struct libusb_transfer *transfer;
};

/* DEBUGGG */
#include <stdio.h>

void print_hex_gn(unsigned char *data, int len, int sz) {
	for (int i = 0; i < len; i++) {
		if ((i % 16) == 0) {
			if (i != 0) {
				printf(" | ");
				for (int j = i-16; j < i; ++j)
					printf("%c", isprint(data[j * sz]) ? data[j * sz] : '.');
				printf("\n");
			}
			printf("%04x ", i);
		} else if ((i % 8) == 0) {
			printf(" ");
		}
		printf("%02x ", data[i * sz]);
	}

	if (((len-1) % 16) != 0) {
		int j;
		int missing_bytes = (15 - (len-1) % 16);
		int missing_spaces = missing_bytes * 3 + (missing_bytes >= 8 ? 1 : 0);

		for (int i = 0; i < missing_spaces; ++i)
			printf(" ");

		printf(" | ");

		for (j = len-1; j > 0 && (j % 16) != 0; --j);
		for (; j < len; ++j)
			printf("%c", isprint(data[j * sz]) ? data[j * sz] : '.');
	}
	puts("");
}

void print_hex_string(char *data, int len) {
	for (int i = 0; i < len; i++) {
		printf("%02x", data[i]);
	}
	puts("");
}

void print_hex(unsigned char *data, int len) {
	print_hex_gn(data, len, 1);
}

/* remove emmmeeme */

static unsigned char *tls_encrypt(struct fp_img_dev *idev,
				  const unsigned char *data, int data_size,
				  int *encrypted_len_out);
static gboolean tls_decrypt(struct fp_img_dev *idev,
			    const unsigned char *buffer, int buffer_size,
			    unsigned char *output_buffer, int *output_len);

typedef void (*async_operation_cb)(struct fp_img_dev *idev, int status, void *data);

struct async_usb_operation_data_t {
	struct fp_img_dev *idev;
	async_operation_cb callback;
	void *callback_data;

	gboolean completed;
};

static gboolean async_transfer_completed(struct fp_img_dev *idev)
{
	struct async_usb_operation_data_t *op_data;
	struct vfs_dev_t *vdev = idev->priv;

	if (!vdev->transfer)
		return TRUE;

	op_data = vdev->transfer->user_data;
	return op_data->completed;
}

static void async_write_callback(struct libusb_transfer *transfer)
{
	struct async_usb_operation_data_t *op_data = transfer->user_data;
	struct fp_img_dev *idev = op_data->idev;
	struct vfs_dev_t *vdev = idev->priv;

	op_data->completed = TRUE;

	if (transfer->status != 0) {
		fp_err("USB write transfer error: %s", libusb_error_name(transfer->status));
		fpi_imgdev_session_error(idev, transfer->status);
		goto out;
	}

	if (transfer->actual_length != transfer->length) {
		fp_err("Written only %d of %d bytes",
		       transfer->actual_length, transfer->length);
		fpi_imgdev_session_error(idev, -EIO);
		goto out;
	}

out:
	vdev->transfer = NULL;

	if (op_data->callback)
		op_data->callback(idev, transfer->status, op_data->callback_data);

	g_free(op_data);
}

static void async_write_to_usb(struct fp_img_dev *idev,
			       const unsigned char *data, int data_size,
			       async_operation_cb callback, void* callback_data)
{
	struct async_usb_operation_data_t *op_data;
	struct vfs_dev_t *vdev = idev->priv;

	g_assert(async_transfer_completed(idev));

	vdev->transfer = libusb_alloc_transfer(0);
	vdev->transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;

	op_data = g_new0(struct async_usb_operation_data_t, 1);
	op_data->idev = idev;
	op_data->callback = callback;
	op_data->callback_data = callback_data;

	libusb_fill_bulk_transfer(vdev->transfer, idev->udev, 0x01,
				  (unsigned char *) data, data_size,
				  async_write_callback, op_data, VFS_USB_TIMEOUT);
	libusb_submit_transfer(vdev->transfer);
}

static void async_read_callback(struct libusb_transfer *transfer)
{
	struct async_usb_operation_data_t *op_data = transfer->user_data;
	struct fp_img_dev *idev = op_data->idev;
	struct vfs_dev_t *vdev = idev->priv;

	vdev->buffer_length = 0;

	if (transfer->status != 0) {
		fp_err("USB read transfer error: %s",
		       libusb_error_name(transfer->status));
		fpi_imgdev_session_error(idev, transfer->status);
		goto out;
	}

	vdev->buffer_length = transfer->actual_length;

out:
	vdev->transfer = NULL;

	if (op_data->callback)
		op_data->callback(idev, transfer->status, op_data->callback_data);

	g_free(op_data);
}

static void async_read_from_usb(struct fp_img_dev *idev, int read_mode,
				unsigned char *buffer, int buffer_size,
				async_operation_cb callback, void* callback_data)
{
	struct async_usb_operation_data_t *op_data;
	struct vfs_dev_t *vdev = idev->priv;

	g_assert(async_transfer_completed(idev));

	vdev->transfer = libusb_alloc_transfer(0);
	vdev->transfer->flags |= LIBUSB_TRANSFER_FREE_TRANSFER;

	op_data = g_new0(struct async_usb_operation_data_t, 1);
	op_data->idev = idev;
	op_data->callback = callback;
	op_data->callback_data = callback_data;

	switch (read_mode) {
	case VFS_READ_INTERRUPT:
		libusb_fill_interrupt_transfer(vdev->transfer, idev->udev, 0x83,
					       buffer, buffer_size,
					       async_read_callback, op_data,
					       VFS_USB_INTERRUPT_TIMEOUT);
		break;
	case VFS_READ_BULK:
		libusb_fill_bulk_transfer(vdev->transfer, idev->udev, 0x81,
					  buffer, buffer_size,
					  async_read_callback, op_data,
					  VFS_USB_TIMEOUT);
		break;
	default:
		g_assert_not_reached();
	}

	libusb_submit_transfer(vdev->transfer);
}

struct async_usb_encrypted_operation_data_t {
	async_operation_cb callback;
	void *callback_data;

	unsigned char *encrypted_data;
	int encrypted_data_size;
};

static void async_write_encrypted_callback(struct fp_img_dev *idev, int status, void *data)
{
	struct async_usb_encrypted_operation_data_t *enc_op = data;

	if (enc_op->callback)
		enc_op->callback(idev, status, enc_op->callback_data);

	free(enc_op->encrypted_data);
	free(enc_op);
}

static void async_write_encrypted_to_usb(struct fp_img_dev *idev,
					const unsigned char *data, int data_size,
					async_operation_cb callback, void* callback_data)
{
	struct async_usb_encrypted_operation_data_t *enc_op;
	unsigned char *encrypted_data;
	int encrypted_data_size;

	encrypted_data = tls_encrypt(idev, data, data_size,
				     &encrypted_data_size);

	enc_op = g_new0(struct async_usb_encrypted_operation_data_t, 1);
	enc_op->callback = callback;
	enc_op->callback_data = callback_data;
	enc_op->encrypted_data = encrypted_data;
	enc_op->encrypted_data_size = encrypted_data_size;

	async_write_to_usb(idev, encrypted_data, encrypted_data_size,
			   async_write_encrypted_callback, enc_op);
}

static void async_read_encrypted_callback(struct fp_img_dev *idev, int status, void *data)
{
	struct async_usb_encrypted_operation_data_t *enc_op = data;
	struct vfs_dev_t *vdev = idev->priv;
	unsigned char decrypted_data[VFS_USB_BUFFER_SIZE];
	int decrypted_data_size;

	if (status == LIBUSB_TRANSFER_COMPLETED &&
	    !tls_decrypt(idev, vdev->buffer, vdev->buffer_length,
			 decrypted_data, &decrypted_data_size)) {
		status = LIBUSB_TRANSFER_ERROR;
	}

	g_free(vdev->buffer);
	vdev->buffer = g_malloc(VFS_USB_BUFFER_SIZE);
	vdev->buffer_length = decrypted_data_size;

	memcpy(vdev->buffer, decrypted_data, decrypted_data_size);

	if (enc_op->callback)
		enc_op->callback(idev, status, enc_op->callback_data);

	free(enc_op->encrypted_data);
	free(enc_op);
}

static void async_read_decrypt_from_usb(struct fp_img_dev *idev, int read_mode,
					unsigned char *buffer, int buffer_size,
					async_operation_cb callback, void* callback_data)
{
	struct async_usb_encrypted_operation_data_t *enc_op;

	enc_op = g_new0(struct async_usb_encrypted_operation_data_t, 1);
	enc_op->callback = callback;
	enc_op->callback_data = callback_data;

	async_read_from_usb(idev, read_mode, buffer, buffer_size,
			    async_read_encrypted_callback, enc_op);
}

struct async_data_exchange_t {
	async_operation_cb callback;
	void* callback_data;

	int exchange_mode;
	unsigned char *buffer;
	int buffer_size;
};

static void on_async_data_exchange_cb(struct fp_img_dev *idev,
				      int status, void *data)
{
	struct async_data_exchange_t *dex = data;

	if (status == LIBUSB_TRANSFER_COMPLETED) {
		if (dex->exchange_mode == DATA_EXCHANGE_PLAIN) {
			async_read_from_usb(idev, VFS_READ_BULK,
					    dex->buffer,
					    dex->buffer_size,
					    dex->callback, dex->callback_data);
		} else if (dex->exchange_mode == DATA_EXCHANGE_ENCRYPTED) {
			async_read_decrypt_from_usb(idev, VFS_READ_BULK,
						    dex->buffer,
						    dex->buffer_size,
						    dex->callback,
						    dex->callback_data);
		}
	} else if (dex->callback) {
		dex->callback(idev, status, dex->callback_data);
	}

	g_free(dex);
}

static void async_data_exchange(struct fp_img_dev *idev, int exchange_mode,
				const unsigned char *data, int data_size,
				unsigned char *buffer, int buffer_size,
				async_operation_cb callback, void* callback_data)
{
	struct async_data_exchange_t *dex;

	dex = g_new0(struct async_data_exchange_t, 1);
	dex->buffer = buffer;
	dex->buffer_size = buffer_size;
	dex->callback = callback;
	dex->callback_data = callback_data;
	dex->exchange_mode = exchange_mode;

	if (dex->exchange_mode == DATA_EXCHANGE_PLAIN) {
		async_write_to_usb(idev, data, data_size,
				   on_async_data_exchange_cb, dex);
	} else if (dex->exchange_mode) {
		async_write_encrypted_to_usb(idev, data, data_size,
					     on_async_data_exchange_cb, dex);
	} else {
		fp_err("Unknown exchange mode selected\n");
		fpi_imgdev_session_error(idev, -EIO);
	}
}

static void async_transfer_callback_with_ssm(struct fp_img_dev *idev,
					     int status, void *data)
{
	struct fpi_ssm *ssm = data;

	if (status == LIBUSB_TRANSFER_COMPLETED) {
		fpi_ssm_next_state(ssm);
	} else {
		fpi_imgdev_session_error(idev, status);
		fpi_ssm_mark_aborted(ssm, status);
	}
}

static void generate_main_seed(struct fp_img_dev *idev) {
	struct vfs_dev_t *vdev = idev->priv;
	char name[NAME_MAX], serial[NAME_MAX];
	FILE *name_file, *serial_file;
	int name_len, serial_len;

	/* The decoding doesn't work properly using generated Seeds yet */
	const unsigned char test_seed[] = "VirtualBox\0" "0";
	vdev->main_seed = g_malloc(sizeof(test_seed));
	memcpy(vdev->main_seed, test_seed, sizeof(test_seed));
	vdev->main_seed_length = sizeof(test_seed);
	return;

	if (!(name_file = fopen(DMI_PRODUCT_NAME_NODE, "r"))) {
		fp_err("Can't open " DMI_PRODUCT_NAME_NODE);
		fpi_imgdev_session_error(idev, -EIO);
	}
	if (!(serial_file = fopen(DMI_PRODUCT_SERIAL_NODE, "r"))) {
		fp_err("Can't open " DMI_PRODUCT_SERIAL_NODE);
		fpi_imgdev_session_error(idev, -EIO);
	}

	if (fscanf(name_file, "%s", name) != 1) {
		fp_err("Can't parse product name from " DMI_PRODUCT_NAME_NODE);
		fpi_imgdev_session_error(idev, -EIO);
	}

	if (fscanf(serial_file, "%s", serial) != 1) {
		fp_err("Can't parse product name from " DMI_PRODUCT_SERIAL_NODE);
		fpi_imgdev_session_error(idev, -EIO);
	}

	name_len = strlen(name);
	serial_len = strlen(serial);
	vdev->main_seed_length = name_len + serial_len + 2;
	vdev->main_seed = g_malloc0(vdev->main_seed_length);

	memcpy(vdev->main_seed, name, name_len + 1);
	memcpy(vdev->main_seed + name_len + 1, serial, serial_len + 1);

	printf("Main seed is\n");
	print_hex(vdev->main_seed, vdev->main_seed_length);

	fclose(name_file);
	fclose(serial_file);
}

static gboolean usb_operation(int error, struct fp_img_dev *idev)
{
	if (error != 0) {
		fp_err("USB operation failed: %s", libusb_error_name(error));
		if (idev) {
			fpi_imgdev_session_error(idev, -EIO);
		}
		return FALSE;
	}

	return TRUE;
}

static gboolean write_to_usb(struct fp_img_dev *idev,
			     const unsigned char *data,
			     int data_size)
{
	int sent, error;

	error = libusb_bulk_transfer(idev->udev, 0x01,
				     (unsigned char *) data, data_size,
				     &sent, VFS_USB_TIMEOUT);

	if (error != 0) {
		fp_err("USB write transfer error:", libusb_error_name(error));
		return FALSE;
	}

	return TRUE;
}

static gboolean read_from_usb(struct fp_img_dev *idev,
			      const unsigned char *buffer,
			      int buffer_size, int *out_read)
{
	int read, error;

	if (out_read)
		*out_read = 0;

	error = libusb_bulk_transfer(idev->udev, 0x81,
				     (unsigned char *) buffer, buffer_size,
				     &read, VFS_USB_TIMEOUT);

	if (error != 0) {
		fp_err("USB read transfer error:", libusb_error_name(error));
		return FALSE;
	}

	if (out_read)
		*out_read = read;

	return TRUE;
}

static PK11Context* hmac_make_context(const unsigned char *key_bytes, int key_len)
{
	CK_MECHANISM_TYPE hmacMech = CKM_SHA256_HMAC;
	PK11SlotInfo *slot = PK11_GetBestSlot(hmacMech, NULL);

	SECItem key;

	key.data = (unsigned char*) key_bytes;
	key.len = key_len;

	PK11SymKey *pkKey = PK11_ImportSymKey(slot, hmacMech, PK11_OriginUnwrap, CKA_SIGN, &key, NULL);

	SECItem param = { .type = siBuffer, .data = NULL, .len = 0 };

	PK11Context* context = PK11_CreateContextBySymKey(hmacMech, CKA_SIGN, pkKey, &param);
	PK11_DigestBegin(context);

	return context;
}

static unsigned char* hmac_compute(const unsigned char *key, int key_len, unsigned char* data, int data_len)
{
	// XXX: REUSE CONTEXT HERE, don't create it all the times
	PK11Context* context = hmac_make_context(key, key_len);
	PK11_DigestOp(context, data, data_len);

	int len = 0x20;
	unsigned char *res = malloc(len);
	PK11_DigestFinal(context, res, &len, len);
	PK11_DestroyContext(context, PR_TRUE);

	return res;
}

static void mac_then_encrypt(unsigned char type, unsigned char *key_block, const unsigned char *data, int data_len, unsigned char **res, int *res_len) {
	unsigned char *all_data, *hmac, *pad;
	const unsigned char iv[] = {0x4b, 0x77, 0x62, 0xff, 0xa9, 0x03, 0xc1, 0x1e, 0x6f, 0xd8, 0x35, 0x93, 0x17, 0x2d, 0x54, 0xef};

	int prefix_len = 5;
	if (type == 0xFF) {
		prefix_len = 0;
	}

    // header for hmac + data + hmac
	all_data = malloc(prefix_len + data_len + 0x20);
	all_data[0] = type; all_data[1] = all_data[2] = 0x03; all_data[3] = (data_len >> 8) & 0xFF; all_data[4] = data_len & 0xFF;
	memcpy(all_data + prefix_len, data, data_len);

	hmac = hmac_compute(key_block + 0x00, 0x20, all_data, prefix_len + data_len);
	memcpy(all_data + prefix_len + data_len, hmac, 0x20);
	free(hmac);

	EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
	EVP_EncryptInit(context, EVP_aes_256_cbc(), key_block + 0x40, iv);
	EVP_CIPHER_CTX_set_padding(context, 0);

	*res_len = ((data_len + 16) / 16) * 16 + 0x30;
	*res = malloc(*res_len);
	memcpy(*res, iv, 0x10);
	int written = 0, wr2, wr3 = 0;

	EVP_EncryptUpdate(context, *res + 0x10, &written, all_data + prefix_len, data_len + 0x20);

	int pad_len = *res_len - (0x30 + data_len);
	if (pad_len == 0) {
		pad_len = 16;
	}
	pad = malloc(pad_len);
	memset(pad, pad_len - 1, pad_len);

	EVP_EncryptUpdate(context, *res + 0x10 + written, &wr3, pad, pad_len);

	EVP_EncryptFinal(context, *res + 0x10 + written + wr3, &wr2);
	*res_len = written + wr2 + wr3 + 0x10;

	free(all_data);
	free(pad);

	EVP_CIPHER_CTX_free(context);
}

static unsigned char *tls_encrypt(struct fp_img_dev *idev,
				  const unsigned char *data, int data_size,
				  int *encrypted_len_out) {
	struct vfs_dev_t *vdev;
	unsigned char *res, *wr;
	int res_len;

	vdev = idev->priv;
	g_assert(vdev->key_block);

	mac_then_encrypt(0x17, vdev->key_block, data, data_size, &res, &res_len);

	wr = malloc(res_len + 5);
	memcpy(wr + 5, res, res_len);
	wr[0] = 0x17; wr[1] = wr[2] = 0x03; wr[3] = res_len >> 8; wr[4] = res_len & 0xFF;

	*encrypted_len_out = res_len + 5;

	free(res);

	return wr;
}

gboolean tls_write_to_usb(struct fp_img_dev *idev, const unsigned char *data, int data_len) {
	unsigned char *encrypted;
	int encrypted_len;
	gboolean ret;

	encrypted = tls_encrypt(idev, data, data_len, &encrypted_len);
	ret = write_to_usb(idev, encrypted, encrypted_len);
	free(encrypted);

	return ret;
}

static gboolean tls_decrypt(struct fp_img_dev *idev,
			    const unsigned char *buffer, int buffer_size,
			    unsigned char *output_buffer, int *output_len)
{
	struct vfs_dev_t *vdev = idev->priv;

	int buff_len = buffer_size - 5;
	const unsigned char *buff = buffer + 5;
	gboolean ret = FALSE;
	*output_len = 0;

	g_assert(vdev->key_block);

	EVP_CIPHER_CTX *context = EVP_CIPHER_CTX_new();
	if (!EVP_DecryptInit(context, EVP_aes_256_cbc(), vdev->key_block + 0x60, buff)) {
		fp_err("Decryption failed, error: %lu, %s",
		       ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto out;
	}
	EVP_CIPHER_CTX_set_padding(context, 0);

	int res_len = buff_len - 0x10;
	int tlen1 = 0, tlen2;
	unsigned char *res = malloc(res_len);
	if (!EVP_DecryptUpdate(context, res, &tlen1, buff + 0x10, res_len)) {
		fp_err("Decryption failed, error: %lu, %s",
		       ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto out;
	}

	if (!EVP_DecryptFinal(context, res + tlen1, &tlen2)) {
		fp_err("Decryption failed, error: %lu, %s",
		       ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto out;
	}

	*output_len = tlen1 + tlen2 - 0x20 - (res[res_len - 1] + 1);
	memcpy(output_buffer, res, *output_len);
	ret = TRUE;

	out:
	EVP_CIPHER_CTX_free(context);

	return ret;
}

gboolean tls_read_from_usb(struct fp_img_dev *idev, unsigned char *output_buffer, int *output_len) {
	unsigned char raw_buff[VFS_USB_BUFFER_SIZE];
	int raw_buff_len;

	if (read_from_usb(idev, raw_buff, sizeof(raw_buff), &raw_buff_len))
		if (tls_decrypt(idev, raw_buff, raw_buff_len, output_buffer, output_len))
			return TRUE;

	return FALSE;
}

static gboolean check_data_exchange(struct vfs_dev_t *vdev, const struct data_exchange_t *dex)
{
	int i;

	if (dex->rsp_length >= 0 && vdev->buffer_length != dex->rsp_length) {
		fp_err("Expected len: %d, but got %d\n",
		       dex->rsp_length, vdev->buffer_length);
		return FALSE;
	} else if (dex->rsp_length > 0 && dex->rsp != NULL) {
		const unsigned char *expected = dex->rsp;

		for (i = 0; i < vdev->buffer_length; ++i) {
			if (vdev->buffer[i] != expected[i]) {
				fp_warn("Reply mismatch, expected at char %d "
					"(actual 0x%x, expected  0x%x)",
					i, vdev->buffer[i], expected[i]);
				return FALSE;
			}
		}
	}

	return TRUE;
}

static gboolean do_data_exchange_sync(struct fp_img_dev *idev, const struct data_exchange_t *dex, int mode)
{
	struct vfs_dev_t *vdev = idev->priv;

 //        g_clear_pointer(&vdev->buffer, g_free);
	// vdev->buffer = g_malloc0(VFS_USB_BUFFER_SIZE);

//DO async operations!
	vdev->buffer_length = 0;
	printf("Sizeof the pointed seq %u\n",dex->msg_length);
	printf("sending data %p, len %u\n",dex->msg,dex->msg_length);

	switch (mode) {
	case DATA_EXCHANGE_PLAIN:
		if (!write_to_usb(idev, dex->msg, dex->msg_length))
			return FALSE;

		if (!read_from_usb(idev, vdev->buffer, VFS_USB_BUFFER_SIZE, &vdev->buffer_length))
			return FALSE;
		break;

	case DATA_EXCHANGE_ENCRYPTED:
		if (!tls_write_to_usb(idev, dex->msg, dex->msg_length))
			return FALSE;

		if (!tls_read_from_usb(idev, vdev->buffer, &vdev->buffer_length))
			return FALSE;
		break;
	}

	printf("Read len is %d, expected %d\n",vdev->buffer_length,dex->rsp_length);

	return check_data_exchange(vdev, dex);
}

struct data_exchange_async_data_t {
	struct fpi_ssm *ssm;
	const struct data_exchange_t *dex;
};

static void on_data_exchange_cb(struct fp_img_dev *idev, int status, void *data)
{
	struct data_exchange_async_data_t *dex_data = data;
	struct vfs_dev_t *vdev = idev->priv;

	if (status == LIBUSB_TRANSFER_COMPLETED &&
	    check_data_exchange(vdev, dex_data->dex)) {
		fpi_ssm_next_state(dex_data->ssm);
	} else {
		fp_err("Initialization failed at state %d", dex_data->ssm->cur_state);
		fpi_imgdev_session_error(idev, -EIO);
		fpi_ssm_mark_aborted(dex_data->ssm, status);
	}

	g_free(dex_data);
}

static void do_data_exchange(struct fpi_ssm *ssm, const struct data_exchange_t *dex, int mode)
{
	struct fp_img_dev *idev = ssm->priv;
	struct vfs_dev_t *vdev = idev->priv;
	struct data_exchange_async_data_t *dex_data;

	dex_data = g_new0(struct data_exchange_async_data_t, 1);
	dex_data->ssm = ssm;
	dex_data->dex = dex;

	async_data_exchange(idev, mode, dex->msg, dex->msg_length,
			    vdev->buffer, VFS_USB_BUFFER_SIZE,
			    on_data_exchange_cb, dex_data);
}

static void send_init_sequence(struct fpi_ssm *ssm, int sequence)
{
	do_data_exchange(ssm, &INIT_SEQUENCES[sequence], DATA_EXCHANGE_PLAIN);
}

static void TLS_PRF2(const unsigned char *secret, int secret_len, char *str,
		     const unsigned char *seed40, int seed40_len,
		     unsigned char *out_buffer, int buffer_len)
{
	int total_len = 0;
	int str_len = strlen(str);
	unsigned char seed[str_len + seed40_len];
	memcpy(seed, str, str_len);
	memcpy(seed + str_len, seed40, seed40_len);
	int seed_len = str_len + seed40_len;
	unsigned char *a = hmac_compute(secret, secret_len, seed, seed_len);

	while (total_len < buffer_len) {
		unsigned char buffer[0x20 + seed_len];
		memcpy(buffer, a, 0x20);
		memcpy(buffer + 0x20, seed, seed_len);

		unsigned char *p = hmac_compute(secret, secret_len, buffer, 0x20 + seed_len);
		memcpy(out_buffer + total_len, p, MIN(0x20, buffer_len - total_len));
		free(p);

		total_len += 0x20;

		unsigned char *t = hmac_compute(secret, secret_len, a, 0x20);
		free(a);
		a = t;
	}
	free(a);
}

static gboolean check_pad(unsigned char *data, int len)
{
    int pad_size = data[len - 1];

    for(int i = 0; i < pad_size; ++i) {
	if (data[len - 1 - i] != pad_size) {
	    return FALSE;
	}
    }

    return TRUE;
}

static void reverse_mem(unsigned char* data, int size)
{
    unsigned char tmp;
    for (int i = 0; i < size / 2; ++i) {
	tmp = data[i];
	data[i] = data[size - 1 - i];
	data[size - 1 - i] = tmp;
    }
}

static gboolean initialize_ecdsa_key(struct vfs_dev_t *vdev, unsigned char *enc_data, int res_len)
{
	int tlen1 = 0, tlen2;
	unsigned char *res = NULL;
	gboolean ret;
	EVP_CIPHER_CTX *context;

	ret = FALSE;
	context = EVP_CIPHER_CTX_new();

	if (!EVP_DecryptInit(context, EVP_aes_256_cbc(), vdev->masterkey_aes, enc_data)) {
		fp_err("Failed to initialize EVP decrypt, error: %lu, %s",
		       ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto out;
	}

	res = malloc(res_len);
	EVP_CIPHER_CTX_set_padding(context, 0);

	if (!EVP_DecryptUpdate(context, res, &tlen1, enc_data + 0x10, res_len)) {
		fp_err("Failed to EVP decrypt, error: %lu, %s",
		       ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto out;
	}

	if (!EVP_DecryptFinal(context, res + tlen1, &tlen2)) {
		fp_err("EVP Final decrypt failed, error: %lu, %s",
		       ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto out;
	}

	reverse_mem(res, 0x20);
	reverse_mem(res + 0x20, 0x20);
	reverse_mem(res + 0x40, 0x20);

	puts("Decoded:");
	print_hex(res, res_len);
	memcpy(vdev->ecdsa_private_key, res, VFS_ECDSA_PRIVATE_KEY_SIZE);

	ret = check_pad(res, res_len);
out:
	EVP_CIPHER_CTX_free(context);
	free(res);

	return ret;
}

static gboolean make_ecdsa_key(struct vfs_dev_t *vdev)
{
	if (!initialize_ecdsa_key(vdev, vdev->buffer + 0x52, 0x70))
		return FALSE;

	memset(vdev->ecdsa_private_key, 0, 0x40);
	// 97 doesn't have XY in private key
	memcpy(vdev->ecdsa_private_key, vdev->buffer + 0x11e, 0x20);
	reverse_mem(vdev->ecdsa_private_key, 0x20);

	memcpy(vdev->ecdsa_private_key + 0x20, vdev->buffer + 0x162, 0x20);
	reverse_mem(vdev->ecdsa_private_key + 0x20, 0x20);

	// ECDSA key
	puts("ECDSA key:");
	print_hex(vdev->ecdsa_private_key, 0x60);

	return TRUE;
}

static EC_KEY *load_key(const unsigned char *data, gboolean is_private)
{
	BIGNUM *x = BN_bin2bn(data, 0x20, NULL);
	BIGNUM *y = BN_bin2bn(data + 0x20, 0x20, NULL);
	BIGNUM *d = NULL;
	EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);

	if (!EC_KEY_set_public_key_affine_coordinates(key, x,y)) {
		fp_err("Failed to set public key coordinates, error: %lu, %s",
		       ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto err;
	}

	if (is_private) {
		d = BN_bin2bn(data + 0x40, 0x20, NULL);
		if (!EC_KEY_set_private_key(key, d)) {
			fp_err("Failed to set private key, error: %lu, %s",
				ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
			goto err;
		}
	}

	if (!EC_KEY_check_key(key)) {
		fp_err("Failed to check key, error: %lu, %s",
			ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
		goto err;
	}

	goto out;

err:
	g_clear_pointer(&key, EC_KEY_free);

out:
	g_clear_pointer(&x, BN_free);
	g_clear_pointer(&y, BN_free);
	g_clear_pointer(&d, BN_free);

	return key;
}

static void fill_buffer_with_random(unsigned char *buffer, int size)
{
	int i;
	srand(time(NULL));

	for (i = 0; i < size; ++i)
		buffer[i] = rand() % 0x100;
}

static unsigned char *sign2(EC_KEY* key, unsigned char *data, int data_len) {
    int len = 0;
    unsigned char *res = NULL;

    do {
	ECDSA_SIG *sig = ECDSA_do_sign(data, data_len, key);
	len = i2d_ECDSA_SIG(sig, NULL);

	free(res);
	res = malloc(len);
	unsigned char *f = res;
	i2d_ECDSA_SIG(sig, &f);
    } while (len != VFS_ECDSA_SIGNATURE_SIZE);

    return res;
}

struct tls_handshake_t {
	struct fp_img_dev *idev;
	struct fpi_ssm *parent_ssm;
	HASHContext *tls_hash_context;
	HASHContext *tls_hash_context2;
	unsigned char read_buffer[VFS_USB_BUFFER_SIZE];
	unsigned char client_random[0x20];
	unsigned char master_secret[0x30];
	unsigned char *client_hello;
};

static void handshake_ssm(struct fpi_ssm *ssm)
{
	struct tls_handshake_t *tlshd = ssm->priv;
	struct fp_img_dev *idev = tlshd->idev;
	struct vfs_dev_t *vdev = idev->priv;

	switch(ssm->cur_state) {
	case TLS_HANDSHAKE_STATE_CLIENT_HELLO:
	{
		time_t current_time;
		unsigned char *client_hello;

		tlshd->tls_hash_context = HASH_Create(HASH_AlgSHA256);
		tlshd->tls_hash_context2 = HASH_Create(HASH_AlgSHA256);

		HASH_Begin(tlshd->tls_hash_context);
		HASH_Begin(tlshd->tls_hash_context2);

		client_hello = malloc(G_N_ELEMENTS(TLS_CLIENT_HELLO));
		tlshd->client_hello = client_hello;

		current_time = time(NULL);
		memcpy(tlshd->client_random, &current_time, sizeof(time_t));
		fill_buffer_with_random(tlshd->client_random + 4, G_N_ELEMENTS(tlshd->client_random) - 4);
		puts("Client random");
		print_hex(tlshd->client_random, sizeof(tlshd->client_random));

		memcpy(client_hello, TLS_CLIENT_HELLO, G_N_ELEMENTS(TLS_CLIENT_HELLO));
		memcpy(client_hello + 0xf, tlshd->client_random, G_N_ELEMENTS(tlshd->client_random));
		HASH_Update(tlshd->tls_hash_context, client_hello + 0x09, 0x43);
		HASH_Update(tlshd->tls_hash_context2, client_hello + 0x09, 0x43);

		puts("Sending Client hello");
		print_hex(client_hello, sizeof(TLS_CLIENT_HELLO));

		async_data_exchange(idev, DATA_EXCHANGE_PLAIN,
				    client_hello, G_N_ELEMENTS(TLS_CLIENT_HELLO),
				    tlshd->read_buffer, sizeof(tlshd->read_buffer),
				    async_transfer_callback_with_ssm, ssm);

		break;
	}
	case TLS_HANDSHAKE_STATE_SERVER_HELLO_RCV:
	{
		unsigned char server_random[0x40];
		unsigned char seed[0x40], expansion_seed[0x40];
		unsigned char *pre_master_secret;
		size_t pre_master_secret_len;

		EC_KEY *priv_key, *pub_key;
		EVP_PKEY_CTX *ctx;
		EVP_PKEY *priv, *pub;

		memcpy(server_random, tlshd->read_buffer + 0xb, G_N_ELEMENTS(server_random));
		puts("Server tls Random:");
		print_hex(server_random, G_N_ELEMENTS(server_random));
		// printf("server len %d\n", len); // remove len!
		HASH_Update(tlshd->tls_hash_context, tlshd->read_buffer + 0x05, 0x3d);
		HASH_Update(tlshd->tls_hash_context2, tlshd->read_buffer + 0x05, 0x3d);

		if (!(priv_key = load_key(PRIVKEY, TRUE))) {
			fp_err("Impossible to load private key");
			fpi_ssm_mark_aborted(ssm, -EIO);
			break;
		}

		if (!(pub_key = load_key(vdev->pubkey, FALSE))) {
			fp_err("Impossible to load private key");
			fpi_ssm_mark_aborted(ssm, -EIO);
			break;
		}

		priv = EVP_PKEY_new();
		EVP_PKEY_set1_EC_KEY(priv, priv_key);
		pub = EVP_PKEY_new();
		EVP_PKEY_set1_EC_KEY(pub, pub_key);

		ctx = EVP_PKEY_CTX_new(priv, NULL);

		EVP_PKEY_derive_init(ctx);
		EVP_PKEY_derive_set_peer(ctx, pub);

		EVP_PKEY_derive(ctx, NULL, &pre_master_secret_len);

		pre_master_secret = malloc(pre_master_secret_len);
		if (!ECDH_compute_key(pre_master_secret, pre_master_secret_len, EC_KEY_get0_public_key(pub_key), priv_key, NULL)) {
			fp_err("Failed to compute key, error: %lu, %s",
			ERR_peek_last_error(), ERR_error_string(ERR_peek_last_error(), NULL));
			fpi_ssm_mark_aborted(ssm, ERR_peek_last_error());
			break;
		}

		memcpy(seed, tlshd->client_random, G_N_ELEMENTS(tlshd->client_random));
		memcpy(seed + G_N_ELEMENTS(tlshd->client_random), server_random, G_N_ELEMENTS(seed) - G_N_ELEMENTS(tlshd->client_random));

		memcpy(expansion_seed + (G_N_ELEMENTS(expansion_seed) - G_N_ELEMENTS(tlshd->client_random)), tlshd->client_random, G_N_ELEMENTS(tlshd->client_random));
		memcpy(expansion_seed, server_random, G_N_ELEMENTS(expansion_seed) - G_N_ELEMENTS(tlshd->client_random));

		TLS_PRF2(pre_master_secret, pre_master_secret_len, "master secret", seed, G_N_ELEMENTS(seed),
			 tlshd->master_secret, G_N_ELEMENTS(tlshd->master_secret));
		puts("master secret");
		print_hex(tlshd->master_secret, G_N_ELEMENTS(tlshd->master_secret));

		TLS_PRF2(tlshd->master_secret, G_N_ELEMENTS(tlshd->master_secret), "key expansion",
			seed, G_N_ELEMENTS(seed), vdev->key_block, G_N_ELEMENTS(vdev->key_block));
		puts("Keyblock");
		print_hex(vdev->key_block, G_N_ELEMENTS(vdev->key_block));

		g_free(pre_master_secret);
		EC_KEY_free(priv_key);
		EC_KEY_free(pub_key);
		EVP_PKEY_free(priv);
		EVP_PKEY_free(pub);
		EVP_PKEY_CTX_free(ctx);

		fpi_ssm_next_state(ssm);

		break;
	}
	case TLS_HANDSHAKE_GENERATE_CERT:
	{
		EC_KEY *ecdsa_key;
		unsigned char test[0x20];
		unsigned char *cert_verify_signature, *final;
		int len, test_len;

		memcpy(vdev->tls_certificate + 0xce + 4, PRIVKEY, 0x40);

		HASH_Update(tlshd->tls_hash_context, vdev->tls_certificate + 0x09, 0x109);
		HASH_Update(tlshd->tls_hash_context2, vdev->tls_certificate + 0x09, 0x109);

		HASH_End(tlshd->tls_hash_context, test, &test_len, G_N_ELEMENTS(test));
		puts("Hash");
		print_hex(test, 0x20);

		ecdsa_key = load_key(vdev->ecdsa_private_key, TRUE);
		cert_verify_signature = sign2(ecdsa_key, test, 0x20);

		printf("\nCert signed: \n");
		print_hex(cert_verify_signature, VFS_ECDSA_SIGNATURE_SIZE);
		memcpy(vdev->tls_certificate + 0x09 + 0x109 + 0x04, cert_verify_signature, VFS_ECDSA_SIGNATURE_SIZE);

		// encrypted finished
		unsigned char handshake_messages[0x20]; int len3 = 0x20;
		HASH_Update(tlshd->tls_hash_context2, vdev->tls_certificate + 0x09 + 0x109, 0x4c);
		HASH_End(tlshd->tls_hash_context2, handshake_messages, &len3, 0x20);

		puts("hash of handshake messages");
		print_hex(handshake_messages, 0x20); // ok

		unsigned char finished_message[0x10] = { 0x14, 0x00, 0x00, 0x0c, 0 };
		print_hex(finished_message, 0x10);
		unsigned char client_finished[0x0c];
		TLS_PRF2(tlshd->master_secret, 0x30, "client finished", handshake_messages, 0x20,
			client_finished, G_N_ELEMENTS(client_finished));
		memcpy(finished_message + 0x04, client_finished, G_N_ELEMENTS(client_finished));
		// copy handshake protocol

		puts("client finished");
		print_hex(finished_message, 0x10);

		mac_then_encrypt(0x16, vdev->key_block, finished_message, 0x10, &final, &len);
		memcpy(vdev->tls_certificate + 0x169, final, len);

		puts("final");
		print_hex(final, len);

		EC_KEY_free(ecdsa_key);

		g_free(cert_verify_signature);
		g_free(final);

		fpi_ssm_next_state(ssm);

		break;
	}
	case TLS_HANDSHAKE_STATE_SEND_CERT:
	{
		async_data_exchange(idev, DATA_EXCHANGE_PLAIN,
				    vdev->tls_certificate,
				    sizeof(vdev->tls_certificate),
				    tlshd->read_buffer, VFS_USB_BUFFER_SIZE,
				    async_transfer_callback_with_ssm, ssm);

		break;
	}
	case TLS_HANDSHAKE_STATE_CERT_REPLY:
	{
		const unsigned char WRONG_TLS_CERT_RSP[] = { 0x15, 0x03, 0x03, 0x00, 0x02 };

		if (vdev->buffer_length < 50 ||
		    memcmp (tlshd->read_buffer, WRONG_TLS_CERT_RSP, MIN(vdev->buffer_length, G_N_ELEMENTS(WRONG_TLS_CERT_RSP))) == 0) {
			print_hex(tlshd->read_buffer, vdev->buffer_length);
			fp_err("TLS Certificate submitted isn't accepted by reader");
			fpi_ssm_mark_aborted(ssm, -EIO);
			break;
		}

		fpi_ssm_next_state(ssm);

		break;
	}
	default:
		fp_err("Unknown state");
		fpi_imgdev_session_error(idev, -EIO);
		fpi_ssm_mark_aborted(ssm, -EIO);
	}
}

static void handshake_ssm_cb(struct fpi_ssm *ssm)
{
	struct tls_handshake_t *tlshd = ssm->priv;
	struct fpi_ssm *parent_ssm = tlshd->parent_ssm;
	struct fp_img_dev *idev = tlshd->idev;

	if (ssm->error) {
		fpi_imgdev_session_error(idev, ssm->error);
		fpi_ssm_mark_aborted(parent_ssm, ssm->error);
	} else {
		fpi_ssm_next_state(parent_ssm);
	}

	HASH_Destroy(tlshd->tls_hash_context);
	HASH_Destroy(tlshd->tls_hash_context2);
	g_clear_pointer(&tlshd->client_hello, g_free);
	g_free(tlshd);
	fpi_ssm_free(ssm);
}

static void start_handshake_ssm(struct fp_img_dev *idev, struct fpi_ssm *parent_ssm)
{
	struct tls_handshake_t *tlshd;

	tlshd = g_new0(struct tls_handshake_t, 1);
	tlshd->idev = idev;
	tlshd->parent_ssm = parent_ssm;

	struct fpi_ssm *ssm =
	    fpi_ssm_new(idev->dev, handshake_ssm, TLS_HANDSHAKE_STATE_LAST);

	ssm->priv = tlshd;
	fpi_ssm_start(ssm, handshake_ssm_cb);
}

/*
int writeImage(char* filename, int width, int height, float *buffer) {
    int code = 0;
    FILE *fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;

    // Open file for writing (binary mode)
    fp = fopen(filename, "wb");
    if (fp == NULL) {
	fprintf(stderr, "Could not open file %s for writing\n", filename);
	code = 1;
	goto finalise;
    }

    // Initialize write structure
    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL) {
	fprintf(stderr, "Could not allocate write struct\n");
	code = 1;
	goto finalise;
    }

    // Initialize info structure
    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL) {
	fprintf(stderr, "Could not allocate info struct\n");
	code = 1;
	goto finalise;
    }

    // Setup Exception handling
    if (setjmp(png_jmpbuf(png_ptr))) {
	fprintf(stderr, "Error during png creation\n");
	code = 1;
	goto finalise;
    }

    png_init_io(png_ptr, fp);

    // Write header (8 bit colour depth)
    png_set_IHDR(png_ptr, info_ptr, width, height,
	    8, PNG_COLOR_TYPE_GRAY, PNG_INTERLACE_NONE,
	    PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);


    png_write_info(png_ptr, info_ptr);

    // Write image data
    int x, y;
    for (y=0 ; y<height ; y++) {
       png_write_row(png_ptr, buffer + 36 * y);
    }

    // End write
    png_write_end(png_ptr, NULL);

    finalise:
    if (fp != NULL) fclose(fp);
    if (info_ptr != NULL) png_free_data(png_ptr, info_ptr, PNG_FREE_ALL, -1);
    if (png_ptr != NULL) png_destroy_write_struct(&png_ptr, (png_infopp)NULL);

    return code;
}
*/
static void fingerprint(struct fp_img_dev *idev) {
    // const unsigned char data1[] = {
    //     0x08, 0x5c, 0x20, 0x00, 0x80, 0x07, 0x00, 0x00, 0x00, 0x04
    // };
    // const unsigned char data2[] = {
    //     0x07, 0x80, 0x20, 0x00, 0x80, 0x04
    // };
    // const unsigned char data345[] = {
    //     0x75
    // };
    // const unsigned char data67[] = {
    //     0x43, 0x02
    // };

    const unsigned char data10[] = {
	0x51, 0x00, 0x20, 0x00, 0x00 // read data - return buffer
    };
    // unsigned char data11[] = {
    //     0x51, 0x00, 0x20, 0x00, 0x00
    // };

    unsigned char response[1024 * 1024];
    int response_len = 0;

    // tls_write_to_usb(idev, data1, sizeof(data1));
    // tls_read_from_usb(idev, response, &response_len);
    // g_print("TLS WRITE "G_STRLOC" response length is %d\n",response_len);
    // // puts("READ:");print_hex(response, response_len);

    // tls_write_to_usb(idev, data2, sizeof(data2));
    // tls_read_from_usb(idev, response, &response_len);
    // g_print("TLS WRITE "G_STRLOC" response length is %d\n",response_len);
    // // puts("READ:");print_hex(response, response_len);

    // for (int i =0; i < 3; i++ ){
    //     tls_write_to_usb(idev, data345, sizeof(data345));
    //     tls_read_from_usb(idev, response, &response_len);
    //     g_print("TLS WRITE "G_STRLOC" response length is %d\n",response_len);
    //     // puts("READ:");print_hex(response, response_len);
    // }

    // for (int i =0; i < 2; i++ ){
    //     tls_write_to_usb(idev, data67, sizeof(data67));
    //     tls_read_from_usb(idev, response, &response_len);
    //     g_print("TLS WRITE "G_STRLOC" response length is %d\n",response_len);
    //     // puts("READ:");print_hex(response, response_len);
    // }
    // g_print("SCAN MATRIX SEND\n");


    // tls_write_to_usb(idev, SCAN_MATRIX2, sizeof(SCAN_MATRIX2));
    // tls_read_from_usb(idev, response, &response_len);
    // g_print("TLS WRITE "G_STRLOC" response length is %d\n",response_len);
    // // puts("READ:");print_hex(response, response_len);

    // tls_write_to_usb(idev, SCAN_MATRIX1, sizeof(SCAN_MATRIX1));
    // tls_read_from_usb(idev, response, &response_len);
    // g_print("TLS WRITE "G_STRLOC" response length is %d\n",response_len);
    // // puts("READ:");print_hex(response, response_len);

    unsigned char interrupt[0x100];
    int interrupt_len;

    const unsigned char desired_interrupt[] = { 0x03, 0x43, 0x04, 0x00, 0x41 };
    const unsigned char other_scan_interrupt[] = { 0x03, 0x42, 0x04, 0x00, 0x40 };
    const unsigned char scan_failed_swipe_interrupt[] = { 0x03, 0x60, 0x07, 0x00, 0x40 };
    const unsigned char scan_failed_toofast_interrupt[] = { 0x03, 0x20, 0x07, 0x00, 0x00 };

    puts("Awaiting fingerprint:");
    while (TRUE) {
	int status = libusb_interrupt_transfer(idev->udev, 0x83, interrupt, 0x100, &interrupt_len, 5 * 1000);
	if (status == 0) {
	    puts("interrupt:");
	    print_hex(interrupt, interrupt_len);
	    fflush(stdout);

	    if (sizeof(desired_interrupt) == interrupt_len &&
		    memcmp(desired_interrupt, interrupt, sizeof(desired_interrupt)) == 0) {
		break;
	    }
	    if (sizeof(other_scan_interrupt) == interrupt_len &&
		    memcmp(other_scan_interrupt, interrupt, sizeof(desired_interrupt)) == 0) {
		printf("ALTERNATIVE SCAN, let's see this result!!!!\n");
		break;
	    }
	    if (sizeof(scan_failed_swipe_interrupt) == interrupt_len &&
		    memcmp(scan_failed_swipe_interrupt, interrupt, sizeof(desired_interrupt)) == 0) {
		puts("scan failed, swipe!");
		return;
	    }
	    if (sizeof(scan_failed_toofast_interrupt) == interrupt_len &&
		    memcmp(scan_failed_toofast_interrupt, interrupt, sizeof(desired_interrupt)) == 0) {
		puts("scan failed, too fast");
		return;
	    }
	}
    }

    unsigned char image[144 * 144];
    int image_len = 0;

    tls_write_to_usb(idev, data10, sizeof(data10));
    tls_read_from_usb(idev, response, &response_len);
    memcpy(image, response + 0x12, response_len - 0x12);
    image_len += response_len - 0x12;

    tls_write_to_usb(idev, data10, sizeof(data10));
    tls_read_from_usb(idev, response, &response_len);
    memcpy(image + image_len, response + 0x06, response_len - 0x06);
    image_len += response_len - 0x06;

    tls_write_to_usb(idev, data10, sizeof(data10));
    tls_read_from_usb(idev, response, &response_len);
    memcpy(image + image_len, response + 0x06, response_len - 0x06);
    image_len += response_len - 0x06;

    printf("total len  %d\n", image_len);
    char nameprefix[80];
    static int number_of_img = 0;
    sprintf(nameprefix, "img %d.png",number_of_img);
    // writeImage(nameprefix, 144, 144, image);

    sprintf(nameprefix, "img %d.raw",number_of_img);
    FILE *f = fopen(nameprefix, "wb");
    fwrite(image, 144, 144, f);
    fclose(f);

    char msg[80];
    sprintf(msg, "Image written - img %d.png, img %d.raw",number_of_img,number_of_img);
    puts(msg);

    ++number_of_img;
}

static int translate_interrupt(unsigned char *interrupt, int interrupt_size)
{
	const unsigned char waiting_finger[] = { 0x00, 0x00, 0x00, 0x00, 0x00 };
	const unsigned char finger_down[] = { 0x02, 0x00, 0x40, 0x10, 0x00 };
	const unsigned char finger_down2[] = { 0x02, 0x00, 0x40, 0x06, 0x06 };
	const unsigned char finger_down3[] = { 0x02, 0x00, 0x40, 0x0e, 0x00 };
	const unsigned char finger_down4[] = { 0x02, 0x00, 0x40, 0x0a, 0x01 };
	const unsigned char scanning_prints[] = { 0x03, 0x40, 0x01, 0x00, 0x00 };
	const unsigned char scan_completed[] = { 0x03, 0x41, 0x03, 0x00, 0x40 };

	const unsigned char desired_interrupt[] = { 0x03, 0x43, 0x04, 0x00, 0x41 };
	const unsigned char low_quality_scan_interrupt[] = { 0x03, 0x42, 0x04, 0x00, 0x40 };
	const unsigned char scan_failed_too_short_interrupt[] = { 0x03, 0x60, 0x07, 0x00, 0x40 };
	const unsigned char scan_failed_too_short2_interrupt[] = { 0x03, 0x61, 0x07, 0x00, 0x41 };
	const unsigned char scan_failed_too_fast_interrupt[] = { 0x03, 0x20, 0x07, 0x00, 0x00 };

	if (sizeof(waiting_finger) == interrupt_size &&
		memcmp(waiting_finger, interrupt, interrupt_size) == 0) {
		fp_info("Waiting for finger...");
		return VFS_SCAN_WAITING_FOR_FINGER;
	}

	if ((sizeof(finger_down) == interrupt_size &&
	     memcmp(finger_down, interrupt, interrupt_size) == 0) ||
	    (sizeof(finger_down2) == interrupt_size &&
	     memcmp(finger_down2, interrupt, interrupt_size) == 0) ||
	    (sizeof(finger_down3) == interrupt_size &&
	     memcmp(finger_down3, interrupt, interrupt_size) == 0) ||
	    (sizeof(finger_down4) == interrupt_size &&
	     memcmp(finger_down4, interrupt, interrupt_size) == 0)) {
		fp_info("Finger is on the sensor...");
		return VFS_SCAN_FINGER_ON_SENSOR;
	}

	if (sizeof(scanning_prints) == interrupt_size &&
	    memcmp(scanning_prints, interrupt, interrupt_size) == 0) {
		fp_info("Scan in progress...");
		return VFS_SCAN_IN_PROGRESS;
	}

	if (sizeof(scan_completed) == interrupt_size &&
	    memcmp(scan_completed, interrupt, interrupt_size) == 0) {
		fp_info("Fingerprint scan completed...");
		return VFS_SCAN_COMPLETED;
	}

	if (sizeof(desired_interrupt) == interrupt_size &&
	    memcmp(desired_interrupt, interrupt, interrupt_size) == 0) {
		fp_info("Fingerprint scan success...");
		return VFS_SCAN_SUCCESS;
	}

	if (sizeof(low_quality_scan_interrupt) == interrupt_size &&
	    memcmp(low_quality_scan_interrupt, interrupt, interrupt_size) == 0) {
		printf("ALTERNATIVE SCAN, let's see this result!!!!\n");
		return VFS_SCAN_SUCCESS_LOW_QUALITY;
	}

	if (sizeof(scan_failed_too_short_interrupt) == interrupt_size &&
	    memcmp(scan_failed_too_short_interrupt, interrupt, interrupt_size) == 0) {
		fp_err("Impossible to read fingerprint, don't move your finger");
		return VFS_SCAN_FAILED_TOO_SHORT;
	}

	if (sizeof(scan_failed_too_short2_interrupt) == interrupt_size &&
	    memcmp(scan_failed_too_short2_interrupt, interrupt, interrupt_size) == 0) {
		fp_err("Impossible to read fingerprint, don't move your finger (2)");
		return VFS_SCAN_FAILED_TOO_SHORT;
	}

	if (sizeof(scan_failed_too_fast_interrupt) == interrupt_size &&
	    memcmp(scan_failed_too_fast_interrupt, interrupt, interrupt_size) == 0) {
		fp_err("Impossible to read fingerprint, movement was too fast");
		return VFS_SCAN_FAILED_TOO_FAST;
	}

	fp_err("Interrupt not tracked, please report!");
	print_hex(interrupt, interrupt_size);

	return VFS_SCAN_UNKNOWN;
}


static int wait_for_finger_state(struct fp_img_dev *idev) {
	unsigned char interrupt[0x100] = {};
	int interrupt_len;


	g_print("Wait for wait_for_finger_state, state is %d\n",idev->action_state);
	while (TRUE) {
		int status = libusb_interrupt_transfer(idev->udev, 0x83, interrupt, 0x100, &interrupt_len, 5 * 1000);
		if (status == 0) {
			puts("interrupt:");
			print_hex(interrupt, interrupt_len);
			fflush(stdout);

			return translate_interrupt(interrupt, interrupt_len);
		}
	}

	return VFS_SCAN_UNKNOWN;
}

static void save_image(struct fp_img_dev *idev)
{
	const unsigned char read_image_request[] = {
		0x51, 0x00, 0x20, 0x00, 0x00 // read data - return buffer
	};

	unsigned char response[VFS_USB_BUFFER_SIZE];
	unsigned char image[144 * 144];
	int image_len = 0;
	int response_len;

	tls_write_to_usb(idev, read_image_request, sizeof(read_image_request));
	tls_read_from_usb(idev, response, &response_len);
	memcpy(image, response + 0x12, response_len - 0x12);
	image_len += response_len - 0x12;

	tls_write_to_usb(idev, read_image_request, sizeof(read_image_request));
	tls_read_from_usb(idev, response, &response_len);
	memcpy(image + image_len, response + 0x06, response_len - 0x06);
	image_len += response_len - 0x06;

	tls_write_to_usb(idev, read_image_request, sizeof(read_image_request));
	tls_read_from_usb(idev, response, &response_len);
	memcpy(image + image_len, response + 0x06, response_len - 0x06);
	image_len += response_len - 0x06;

	printf("total len  %d\n", image_len);
	char nameprefix[80];
	static int number_of_img = 0;
	sprintf(nameprefix, "img %d.png",number_of_img);

	sprintf(nameprefix, "img %d.raw",number_of_img);
	FILE *f = fopen(nameprefix, "wb");
	fwrite(image, 144, 144, f);
	fclose(f);

	char msg[80];
	sprintf(msg, "Image written - img %d.png, img %d.raw",number_of_img,number_of_img);
	puts(msg);

	struct fp_img *img = fpi_img_new(sizeof(image));
	memcpy(img->data, image, sizeof(image));
	fpi_imgdev_image_captured(idev, img);

	++number_of_img;
}

/* Main SSM loop */
static void init_ssm(struct fpi_ssm *ssm)
{
	struct fp_img_dev *idev = ssm->priv;
	struct vfs_dev_t *vdev = idev->priv;

	switch (ssm->cur_state) {
	case INIT_STATE_SEQ_1:
	case INIT_STATE_SEQ_2:
	case INIT_STATE_SEQ_3:
	case INIT_STATE_SEQ_4:
	case INIT_STATE_SEQ_5:
	case INIT_STATE_SEQ_6:
		send_init_sequence(ssm, ssm->cur_state - INIT_STATE_SEQ_1);
		break;

	case INIT_STATE_MASTER_KEY:
		puts("prf seed");
		print_hex(vdev->main_seed, vdev->main_seed_length);

		TLS_PRF2(PRE_KEY, sizeof(PRE_KEY), "GWK", vdev->main_seed,
			vdev->main_seed_length,
			vdev->masterkey_aes, VFS_MASTER_KEY_SIZE);

		puts("AES master:");
		print_hex(vdev->masterkey_aes, VFS_MASTER_KEY_SIZE);

		fpi_ssm_next_state(ssm);
		break;

	case INIT_STATE_ECDSA_KEY:
		if (make_ecdsa_key(vdev)) {
			fpi_ssm_next_state(ssm);
		} else {
			fp_err("Initialization failed at state %d, ECDSA key generation",
			       ssm->cur_state);
			fpi_imgdev_session_error(idev, -EIO);
			fpi_ssm_mark_aborted(ssm, -EIO);
		}
		break;

	case INIT_STATE_TLS_CERT:
		memcpy(vdev->tls_certificate + 21, vdev->buffer + 0x116, 0xb8);

		fpi_ssm_next_state(ssm);
		break;

	case INIT_STATE_PUBLIC_KEY:
	{
		const int half_key = VFS_PUBLIC_KEY_SIZE / 2;
		memcpy(vdev->pubkey, vdev->buffer + 0x600 + 10, half_key);
		memcpy(vdev->pubkey + half_key, vdev->buffer + 0x640 + 0xe, half_key);

		reverse_mem(vdev->pubkey, half_key);
		reverse_mem(vdev->pubkey + half_key, half_key);

		puts("pub key:");
		print_hex(vdev->pubkey, VFS_PUBLIC_KEY_SIZE);
		fpi_ssm_next_state(ssm);
		break;
	}
	case INIT_STATE_HANDSHAKE:
		start_handshake_ssm(idev, ssm);
		break;
	default:
		fp_err("Unknown state");
		fpi_imgdev_session_error(idev, -EIO);
		fpi_ssm_mark_aborted(ssm, -EIO);
	}
}

/* Callback for dev_open ssm */
static void dev_open_callback(struct fpi_ssm *ssm)
{
	/* Notify open complete */
	struct fp_img_dev *idev = ssm->priv;
	struct vfs_dev_t *vdev = idev->priv;

	g_clear_pointer(&vdev->buffer, g_free);
	vdev->buffer_length = 0;

	if (ssm->error)
		fpi_imgdev_session_error(idev, ssm->error);

	fpi_imgdev_open_complete(idev, ssm->error);

	fpi_ssm_free(ssm);
}

/* Open device */
static int dev_open(struct fp_img_dev *idev, unsigned long driver_data)
{
	SECStatus secs_status;

	/* Claim usb interface */
	int error = libusb_claim_interface(idev->udev, 0);
	if (error < 0) {
		/* Interface not claimed, return error */
		fp_err("could not claim interface 0");
		return error;
	}

	printf("Opening %p\n",idev->udev);

	secs_status = NSS_NoDB_Init(".");
	if (secs_status != SECSuccess) {
		fp_err("could not initialise NSS");
		return -1;
	}

	OpenSSL_add_all_algorithms();
	ERR_load_crypto_strings();

	/* Initialize private structure */
	struct vfs_dev_t *vdev = g_malloc0(sizeof(struct vfs_dev_t));
	idev->priv = vdev;

	vdev->buffer = g_malloc(VFS_USB_BUFFER_SIZE);
	vdev->buffer_length = 0;
	memcpy(vdev->tls_certificate, TLS_CERTIFICATE_BASE,
	       G_N_ELEMENTS(TLS_CERTIFICATE_BASE));

	usb_operation(libusb_reset_device(idev->udev), idev);
	usb_operation(libusb_set_configuration(idev->udev, 1), idev);
	usb_operation(libusb_claim_interface(idev->udev, 0), idev);

	generate_main_seed(idev);

	/* Clearing previous device state */
	struct fpi_ssm *ssm = fpi_ssm_new(idev->dev, init_ssm, INIT_STATE_LAST);
	ssm->priv = idev;
	fpi_ssm_start(ssm, dev_open_callback);

	return 0;
}

/* Callback for dev_activate ssm */
static void dev_activate_callback(struct fpi_ssm *ssm)
{
	struct fp_img_dev *idev = ssm->priv;
	struct vfs_dev_t *vdev = idev->priv;

	if (ssm->error) {
		fp_err("Activation failed failed at state %d, unexpected"
		       "device reply during initialization", ssm->cur_state);
		fpi_imgdev_session_error(idev, ssm->error);
	}

	g_clear_pointer(&vdev->buffer, g_free);
	vdev->buffer_length = 0;

	fpi_imgdev_activate_complete(idev, ssm->error);
	fpi_ssm_free(ssm);
}

static gboolean send_activate_sequence_sync(struct fp_img_dev *idev, int sequence)
{
	return do_data_exchange_sync(idev, &ACTIVATE_SEQUENCES[sequence], DATA_EXCHANGE_ENCRYPTED);
}

static void send_activate_sequence(struct fpi_ssm *ssm, struct fp_img_dev *idev, int sequence)
{
	do_data_exchange(ssm, &ACTIVATE_SEQUENCES[sequence], DATA_EXCHANGE_ENCRYPTED);
}

static void activate_ssm(struct fpi_ssm *ssm)
{
	struct fp_img_dev *idev = ssm->priv;
	// struct vfs_dev_t *vdev = idev->priv;

	switch (ssm->cur_state) {
	case ACTIVATE_STATE_SEQ_1:
	case ACTIVATE_STATE_SEQ_2:
	case ACTIVATE_STATE_SEQ_3:
	case ACTIVATE_STATE_SEQ_4:
	case ACTIVATE_STATE_SEQ_5:
	case ACTIVATE_STATE_SEQ_6:
	case ACTIVATE_STATE_SEQ_7:
	case ACTIVATE_STATE_SCAN_MATRIX:
		printf("Activate State %d\n",ssm->cur_state);
		send_activate_sequence(ssm, idev, ssm->cur_state - ACTIVATE_STATE_SEQ_1);

		break;

	case ACTIVATE_STATE_WAIT_DEVICE:
		if (wait_for_finger_state(idev) == VFS_SCAN_WAITING_FOR_FINGER) {
			fpi_ssm_next_state(ssm);
		} else {
			fp_err("Activation failed failed at state %d, unexpected"
			       "device reply during initialization", ssm->cur_state);
			fpi_imgdev_session_error(idev, -EIO);
			fpi_ssm_mark_aborted(ssm, -EIO);
		}

		break;

	default:
		fp_err("Unknown state");
		fpi_imgdev_session_error(idev, -EIO);
		fpi_ssm_mark_aborted(ssm, -EIO);
	}
}

static int dev_activate(struct fp_img_dev *idev, enum fp_imgdev_state state)
{
	struct vfs_dev_t *vdev = idev->priv;
	struct fpi_ssm *ssm;

	// SEE IF CAN BE DONE ONLY ON CERTAIN CASES
	vdev->buffer = g_malloc(VFS_USB_BUFFER_SIZE);
	vdev->buffer_length = 0;

	ssm = fpi_ssm_new(idev->dev, activate_ssm, ACTIVATE_STATE_LAST);
	ssm->priv = idev;
	fpi_ssm_start(ssm, dev_activate_callback);

	return 0;
}

static int dev_change_state(struct fp_img_dev *idev, enum fp_imgdev_state state)
{
	int finger_state;
	printf("DEV STATE CHANGE TO %d\n",state);

	switch (state) {
	case IMGDEV_STATE_INACTIVE:
		g_print("IMGDEV_STATE_INACTIVE\n");
		break;
	case IMGDEV_STATE_AWAIT_FINGER_ON:
		g_print("IMGDEV_STATE_AWAIT_FINGER_ON\n");
		while (TRUE) {
			finger_state = wait_for_finger_state(idev);

			if (finger_state == VFS_SCAN_FINGER_ON_SENSOR) {
				fpi_imgdev_report_finger_status(idev, TRUE);
				break;
			}
		}
		break;

	case IMGDEV_STATE_AWAIT_FINGER_OFF:
		g_print("IMGDEV_STATE_AWAIT_FINGER_OFF\n");
		break;
	case IMGDEV_STATE_CAPTURE:
		g_print("IMGDEV_STATE_CAPTURE\n");
		while (TRUE) {
			finger_state = wait_for_finger_state(idev);

			if (finger_state == VFS_SCAN_FAILED_TOO_FAST ||
			    finger_state == VFS_SCAN_FAILED_TOO_SHORT) {
				fpi_imgdev_abort_scan(idev, FP_VERIFY_RETRY_TOO_SHORT);
				// fpi_imgdev_abort_scan(idev, FP_VERIFY_RETRY_CENTER_FINGER);
				// fpi_imgdev_abort_scan(dev, FP_VERIFY_RETRY); // other errors
				fpi_imgdev_report_finger_status(idev, FALSE);
				break;
			} else if (finger_state == VFS_SCAN_SUCCESS ||
				   finger_state == VFS_SCAN_SUCCESS_LOW_QUALITY)
			{
				save_image(idev);
				fpi_imgdev_report_finger_status(idev, FALSE);
				break;
			}
		}
		break;
	default:
		fp_err("unrecognised state %d", state);
		return -EINVAL;
	}

	return 0;
}

static void dev_deactivate(struct fp_img_dev *idev)
{
	// g_clear_pointer(&vdev->buffer, g_free);
}

static void dev_close(struct fp_img_dev *idev)
{
	struct vfs_dev_t *vdev = idev->priv;

	usb_operation(libusb_release_interface(idev->udev, 0), NULL);

	g_clear_pointer(&vdev->buffer, g_free);
	vdev->buffer_length = 0;

	g_free(idev->priv);
	libusb_release_interface(idev->udev, 0);
	fpi_imgdev_close_complete(idev);
}

/* Usb id table of device */
static const struct usb_id id_table[] = {
	{.vendor = 0x138a,.product = 0x0090},
	{0, 0, 0,},
};

/* Device driver definition */
struct fp_img_driver vfs0090_driver = {
	/* Driver specification */
	.driver = {
		.id = VFS0090_ID,
		.name = FP_COMPONENT,
		.full_name = "Validity VFS0090",
		.id_table = id_table,
		.scan_type = FP_SCAN_TYPE_PRESS,
	},

	/* Image specification */
	.flags = 0,
	.img_width = VFS_IMAGE_SIZE,
	.img_height = VFS_IMAGE_SIZE,
	.bz3_threshold = 0,

	/* Routine specification */
	.open = dev_open,
	.close = dev_close,
	.activate = dev_activate,
	.change_state = dev_change_state,
	.deactivate = dev_deactivate,
};
