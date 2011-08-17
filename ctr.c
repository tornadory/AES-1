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
 * [ciphertext block #1], 16 bytes
 * [ciphertext block #2], 16 bytes
 * [ciphertext block #n], 16 bytes
 * [padding byte, 1 byte]
 * The value of the last byte (0 - 15) indicates how many bytes of the last ciphertext block are padding bytes
 * (and should be discarded after decryption).
 *
 * The counter starts at 1 and increases by one for each block that is read.
 */

off_t file_size(const char *path) {
	// Returns an integer-type variable containing the file size, in bytes.
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

	// Expand the keys; AES-128 uses 11 keys (11*16 = 176 bytes) for encryption/decryption, one per round plus one before the rounds
	unsigned char expanded_keys[176] = {0};
	aes_expand_key(key, expanded_keys);

	// Sanity check: don't try to encrypt nothingness (or weird errors stemming from the signed type)
	off_t size = file_size(inpath);
	if (size <= 0) {
		fprintf(stderr, "Cannot encrypt a file of size zero!\n");
		exit(1);
	}

	// Since we can only encrypt full 16-byte blocks, we need to add padding to the last block
	// if its length isn't divisble by 16. This calculates how many padding bytes are needed
	// (in the range 0 - 15).
	uint8_t padding = 16 - (size % 16);
	if (padding == 16)
		padding = 0;

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

	// The plaintext block
	unsigned char block[16] = {0};

	// Where we store the ciphertext
	unsigned char enc_block[16] = {0};

	// The counter (since this is CTR mode)
	// The layout is simple: the first 64 bits is the nonce, and the second 64 bits is a simple counter.
	// This should work for a maximum 2^64-1 blocks, which is 256 exabytes, so there's no need for a 128-bit counter.
	uint64_t counter[2];
	counter[0] = get_nonce();
	counter[1] = 1;

	// Prepend the nonce to the output file; it's needed for decryption, and doesn't need to be a secret
	fwrite((void *)&(counter[0]), 8, 1, outfile);
	// Also prepend the padding byte, so that EOF is the end of data
	fputc(padding, outfile);

	assert(ftell(infile) == 0);
	assert(ftell(outfile) == 9);


#define BUFSIZE 32 // FIXME, 4MB? Make sure it's divisible by 16!
	// TODO: ENSURE than all sizes work without problems! Especially input less than bufsize, 1-15 bytes larger than bufsize, 16 to BUFSIZE-1 bytes larger, and 2 times or more times BUFSIZE
	unsigned char *in_buf = malloc(BUFSIZE);
	if (!in_buf) {
		fprintf(stderr, "Failed to allocate memory for the input buffer!\n");
		exit(1);
	}

	unsigned char *out_buf = malloc(BUFSIZE);
	if (!out_buf) {
		fprintf(stderr, "Failed to allocate memory for the output buffer!\n");
		exit(1);
	}

	// How many bytes we read this chunk
	size_t b = 0;

	// The main loop.
	while(!feof(infile)) {

		b = fread(in_buf, 1, BUFSIZE, infile);
		if (b != BUFSIZE && !feof(infile)) {
			fprintf(stderr, "Read error! Aborting!\n");
			unlink(outpath); // Since we've already truncated it, and it's probably useless incomplete, let's delete it.
			exit(1);
		}

		// Encrypt all the "whole" blocks in this chunk; if there's a partial block at the end, we
		// take care of that after this loop.
		int cur_block = 0; // this is needed (well, it makes things easy) in the if statement just outside the loop
		for (cur_block = 0; cur_block < b/16; cur_block++) {
			// TODO: we don't need to copy the data to the "block" array, do we? why not use the output array?
			memcpy(block, in_buf + (cur_block * 16), 16);
			aes_encrypt((unsigned char *)counter, enc_block, expanded_keys);
			counter[1]++;
			AddRoundKey(enc_block, block);
			memcpy(out_buf + cur_block*16, enc_block, 16);
		}

		// This goes AFTER the inside loop, since the loop will encrypt all "full" blocks but never partial ones.
		if (b % 16 != 0 && feof(infile)) {
			// Pad + encrypt the last block
			memcpy(block, in_buf + (cur_block * 16), 16-padding); // the last block should be 16-padding bytes long
			memset(block + (16-padding), 'A', padding);
			aes_encrypt((unsigned char *)counter, enc_block, expanded_keys);
			counter[1]++;
			AddRoundKey(enc_block, block);
			memcpy(out_buf + (cur_block * 16), enc_block, 16);
		}

		// Write the encrypted chunk
		// The math here could probably be prettier, but this is rather simple!
		// The loop above loops until b/16, which is, for example, 3.125 for a 50-byte input with a 48-byte blocksize (as an example!).
		// Then, because of int truncation, this expression evaluates to (50/16)*16 + 16 = 64 bytes - the length of the ciphertext after padding.
		if (fwrite(out_buf, 1, (b/16)*16 + 16, outfile) != (b/16)*16 + 16) {
			fprintf(stderr, "*** Write error!\n");
			exit(1);
		}
	}

	fclose(infile);
	fclose(outfile);
	free(in_buf); in_buf = NULL;
	free(out_buf); out_buf = NULL;
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

	// Perform key expansion (AES needs 11 keys; one for whitening and one per round - AES-128 has 10 rounds)
	unsigned char expanded_keys[176] = {0};
	aes_expand_key(key, expanded_keys);
	// Note to self: no need to call aes_prepare_decryption_keys since we use aes_ENcrypt for decryption as well

	// Perform some size sanity checking: the smallest possible encryption length is 1 byte, which is padded to 16 bytes; after that,
	// the nonce (8 bytes) and padding byte (1 byte) is added, making the smallest possible ciphertext file 25 bytes.
	off_t size = file_size(inpath);
	if (size < 25) {
		fprintf(stderr, "Invalid file; all files encrypted with this program are 25 bytes or longer.\n");
		exit(1);
	}
	
	// Since all ciphertext comes in blocks of 16, and there are 9 extra bytes, size-9 must be divisible by the block length (16)
	// for this file to have been encrypted with this program.
	if (! ( (size-1-8) % 16 == 0)) {
		fprintf(stderr, "Invalid file size; file is either not encrypted by this program, or corrupt.\n");
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

	off_t bytes_read = 0;
	unsigned char block[16] = {0};
	unsigned char dec_block[16] = {0};
	size_t actual_read = 0;

	uint64_t counter[2];
	fread((void *)&(counter[0]), 8, 1, infile); // read nonce from file
	counter[1] = 1; // initialize counter

	// Read the padding byte
	uint8_t padding;
	fread(&padding, 1, 1, infile);
	printf("padding is %hhu bytes\n", padding);

	bytes_read = 9; // nonce is 8 bytes, padding byte is 1

	size_t bytes_to_write = 16;

	while (bytes_read < size) {
		memset(block, 'a', 16);
		actual_read = fread(block, 1, 16, infile);
		bytes_read += actual_read;
		if (actual_read != 16) { // all ciphertext blocks are 16 bytes; other values means something bad
			fprintf(stderr, "*** Some sort of read error occured\n");
			exit(1);
		}

//		if (actual_read == 16) { // we found another block
			aes_encrypt((unsigned char *)counter, dec_block, expanded_keys);
			counter[1]++;

			// XOR the counter and the ciphertext together to create the plaintext
			// AddRoundKey does exactly this
			AddRoundKey(dec_block, block);

			if (bytes_read == size) {
				// This is the very last block - the one with the padding, if there is any
				if (padding != 0) {
					bytes_to_write = 16 - padding;
				}
			}

			if (fwrite(dec_block, 1, bytes_to_write, outfile) != bytes_to_write) {
				fprintf(stderr, "*** Write error!\n");
				exit(1);
			}
//		}
	}

	fclose(infile);
	fclose(outfile);
}

int main(int argc, char *argv[]) {
	unsigned char key[] = {0x2d, 0x7e, 0x86, 0xa3, 0x39, 0xd9, 0x39, 0x3e, 0xe6, 0x57, 0x0a, 0x11, 0x01, 0x90, 0x4e, 0x16};

	if (argc != 5 || strcmp(argv[3], "-o") != 0) {
		fprintf(stderr, "The arguments MUST be in the form of -d <infile> -o <outfile> *OR* -e <infile> -o <outfile>");
		exit(1);
	}
	if (strcmp(argv[1], "-e") == 0)
		encrypt_file(argv[2], argv[4], key);
	else if (strcmp(argv[1], "-d") == 0)
		decrypt_file(argv[2], argv[4], key);
	else {
		fprintf(stderr, "Need an argument: either -e or -d\n");
		exit(1);
	}

	return 0;
}
