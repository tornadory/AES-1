#include <stdio.h>
#include <stdlib.h> /* exit */
#include <string.h> /* memcmp */
#include <assert.h>
#include <sys/stat.h>
#include <errno.h>

#include "keyschedule.h"
#include "aes.h"
#include "debug.h"
#include "misc.h" /* test_aesni_support */

/*
 * The file structure used by this program is quite simple:
 * [nonce, 8 bytes]
 * [ciphertext block #1]
 * [ciphertext block #2]
 * [ciphertext block #n]
 * [padding byte, 1 byte]
 * The value of the last byte (0 - 15) indicates how many bytes of the last ciphertext block are padding bytes
 * (and should be discarded after decryption).
 *
 * The counter starts at 1 and increases by one for each block that is read.
 */

off_t file_size(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0) {
		perror(path);
		exit(1);
	}

	return (st.st_size);
}

uint64_t get_nonce(void) {
	// Fetches 64 bits of pseudorandom data from /dev/urandom and
	// returns it as a 64-bit integer.

	uint64_t nonce;
	FILE *urandom = fopen("/dev/urandom", "r");
	if (!urandom) {
		perror(NULL);
		exit(1);
	}
	if (fread(&nonce, 8, 1, urandom) != 1) {
		perror(NULL);
		exit(1);
	}

	return nonce;
}

void encrypt_file(const char *inpath, const char *outpath, const unsigned char *key) {

	// Create a pointer to the correct function to use for this CPU
	void (*aes_encrypt)(const unsigned char *, unsigned char *, const unsigned char *);
	if (test_aesni_support()) {
		aes_encrypt = aes_encrypt_aesni;
	}
	else {
		aes_encrypt = aes_encrypt_c;
	}

	unsigned char expanded_keys[176] = {0};
	aes_expand_key(key, expanded_keys);

	off_t size = file_size(inpath);
	if (size <= 0) {
		fprintf(stderr, "Cannot encrypt a file of size zero!\n");
		exit(1);
	}

	uint8_t padding = 16 - (size % 16);

	printf("File to encrypt is %llu bytes; padding needed is %d bytes\n", file_size(inpath), (int)padding);

	FILE *infile = fopen(inpath, "r");
	if (!infile) {
		perror(inpath);
		exit(1);
	}

	FILE *outfile = fopen(outpath, "w");
	if (!outfile) {
		perror(outpath);
		exit(1);
	}

	off_t bytes_read = 0;
	unsigned char block[16] = {0};
	unsigned char enc_block[16] = {0};
	size_t actual_read = 0;


	uint64_t counter[2];
	counter[0] = get_nonce();
	printf("nonce = %llx\n", counter[0]);
	counter[1] = 1;

	printf("\"");

	fwrite((void *)&(counter[0]), 8, 1, outfile); // prepend nonce to the output file, so that we can use it for decryption later

	while (bytes_read < size) {
		memset(block, 'a', 16);
		actual_read = fread(block, 1, 16, infile);
		bytes_read += actual_read;
		if (actual_read != 16) {
			if (bytes_read - actual_read /* total bytes read *BEFORE* the last fread() */
					!=
					size - (16-padding)) { /* number of bytes that should be read in 16-byte blocks */
				fprintf(stderr, "*** Some sort of read error occured.\n");
				exit(1);
			}
			else {
				// Add padding
				memset(block + (16-padding), 'A', padding);
			}
		}

		aes_encrypt((unsigned char *)counter, enc_block, expanded_keys);
		counter[1]++;

		for (int i=0; i<16; i++) {
			enc_block[i] ^= block[i];
		}

		if (fwrite(enc_block, 1, 16, outfile) != 16) {
			fprintf(stderr, "*** Write error!\n");
			exit(1);
		}

		printf("%16s", block);
	}

	fclose(infile);
	fputc(padding, outfile); // write a final byte, whose value is the amount of padding used
	fclose(outfile);

	printf("\"\n");

}

void decrypt_file(const char *inpath, const char *outpath, const unsigned char *key) {
	// Create a pointer to the correct function to use for this CPU
	// [sic] on the aes_ENcrypt - CTR mode uses encryption for both ways (thanks to the fact that a XOR b XOR b == a)
	void (*aes_encrypt)(const unsigned char *, unsigned char *, const unsigned char *);
	if (test_aesni_support()) {
		aes_encrypt = aes_encrypt_aesni;
	}
	else {
		aes_encrypt = aes_encrypt_c;
	}

	unsigned char expanded_keys[176] = {0};
	aes_expand_key(key, expanded_keys);
	// Note to self: no need to call aes_prepare_decryption_keys since we use aes_ENcrypt here

	off_t size = file_size(inpath);
	if (size < 25) {
		fprintf(stderr, "Invalid file; all files encrypted with this program are 25 bytes or longer.\n");
		exit(1);
	}

	FILE *infile = fopen(inpath, "r");
	if (!infile) {
		perror(inpath);
		exit(1);
	}

	FILE *outfile = fopen(outpath, "w");
	if (!outfile) {
		perror(outpath);
		exit(1);
	}

	uint8_t padding;
	fseek(infile, -1, SEEK_END); // seek to the last byte
	fread(&padding, 1, 1, infile); // read the padding byte
	fseek(infile, 0, 0); // seek to the beginning of the file

	printf("padding byte is %hhu\n", padding);

	off_t bytes_read = 0;
	unsigned char block[16] = {0};
	unsigned char dec_block[16] = {0};
	size_t actual_read = 0;

	uint64_t counter[2];
	fread((void *)&(counter[0]), 8, 1, infile); // read nonce from file
	printf("nonce = %llx\n", counter[0]);
	counter[1] = 1; // initialize counter

	bytes_read = 8; // nonce is 8 bytes

	size_t bytes_to_write = 16;

	while (bytes_read < size) {
		memset(block, 'a', 16);
		actual_read = fread(block, 1, 16, infile);
		bytes_read += actual_read;
		printf("actual read: %d\nbytes read total: %d\n", (int)actual_read, (int)bytes_read);
		if (actual_read != 16 && actual_read != 1) { // all blocks are 16 bytes, padding byte is 1 byte; other values means something bad
			fprintf(stderr, "*** Some sort of read error occured\n");
			exit(1);
		}

		if (actual_read == 16) { // we found another block
			aes_encrypt((unsigned char *)counter, dec_block, expanded_keys);
			counter[1]++;

			for (int i=0; i<16; i++) {
				dec_block[i] ^= block[i];
			}

			if (bytes_read == size - 1) {
				// This is the very last block - the one with the padding, if there is any
				if (padding != 0) {
					bytes_to_write = 16 - padding;
				}
			}

			if (fwrite(dec_block, 1, bytes_to_write, outfile) != bytes_to_write) {
				fprintf(stderr, "*** Write error!\n");
				exit(1);
			}
		}
	}

	fclose(infile);
	fclose(outfile);
}

int main(int argc, char *argv[]) {
	unsigned char key[] = {0x2d, 0x7e, 0x86, 0xa3, 0x39, 0xd9, 0x39, 0x3e, 0xe6, 0x57, 0x0a, 0x11, 0x01, 0x90, 0x4e, 0x16};

	if (argc != 2) {
		fprintf(stderr, "Need an argument: either -e or -d\n");
		exit(1);
	}
	if (strcmp(argv[1], "-e") == 0)
		encrypt_file("/Users/serenity/Programming/AES/testing/plaintext", "/Users/serenity/Programming/AES/testing/ciphertext", key);
	else if (strcmp(argv[1], "-d") == 0)
		decrypt_file("/Users/serenity/Programming/AES/testing/ciphertext", "/Users/serenity/Programming/AES/testing/dec_plaintext", key);
	else {
		fprintf(stderr, "Need an argument: either -e or -d\n");
		exit(1);
	}

	return 0;
}
