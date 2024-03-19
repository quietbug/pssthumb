#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "placeholder.h"

// Setup initial storage for 16x16 image
#define INITIAL_SIZE 256
// Bomb protection
#define MAX_RES 10000
// Header in bytes
#define PSS_HEAD 40
// Paintstorm uses 8 bit RGB
#define MAXCOL 255
// Color escape codes
#define RED "\033[0;31m"
#define CRESET "\033[0m"
// debug flags
#undef DEBUG
#undef DEBUG_VERBOSE

// Channel container
typedef struct {
	unsigned int size;
	unsigned int capacity;
	unsigned char* array;
} ch_buf;

// Read next two bytes and swap endianess
unsigned short uswap16(char* ptr);
// Read next two bytes as is
unsigned short ucomb16(char* ptr);

// Initialize container
void u_init(ch_buf** arr_ptr);
// Print contents
void u_print(ch_buf* container);
// Add value to end 
void u_append(ch_buf* container, unsigned char value);
// Get value at index
int  u_getat(ch_buf* container, unsigned int index);

// Print placeholder thumbnail
void print_placeholder();

int main( int argc, const char** argv )
{
	int              fd;                                 // file descriptor
	int              fsize;                              // file size
	int              bytes_read;                         // read() output
	char*            buf;                                // buffer to read file into
	char*            comp_img;                           // buffer for rle compressed image
	ch_buf*          image[3];                           // decompressed image arrays
	unsigned short*  compdata_for_channel[3];            // rle decompression data
	unsigned int     res_h;                              // image height
	unsigned int     res_w;                              // image width
	unsigned int     rle_header_size;                    // byte size of compression info
	unsigned int     rle_channel_length;                 // amount of bytes to read for each row
	const char       * ptr_fname;                        // ptr to file name
	const char       fsig[4] = {0x6A, 0x87, 0x01, 0x00}; // pss specific file signature
	unsigned int rle_total_encoded_size = 0;             // total rle encoded data size in bytes
	unsigned int rle_channel_size[3] = {0, 0, 0};        // size of rle encoded channel in rle block
#ifdef DEBUG
	const char       statfn[] = "input.pss";             // hardcoded for testing
#endif

	// parse args
	if (argc == 2) 
		ptr_fname = argv[1];
	else if (argc == 1) {
		fprintf(stderr, "This program extracts composite image from Paintstorm Studio document\n");
		fprintf(stderr, "and writes it as PPM image to standard output.\n");
		fprintf(stderr, "It can then be converted using imagemagick or netpbm in desired format.\n");
		fprintf(stderr, "   ***\n");
		fprintf(stderr, " PSS format info:\n");
		fprintf(stderr, "* first 40 bytes of file is a software specific header,\n");
		fprintf(stderr, "* it is followed by RLE information block,\n");
		fprintf(stderr, "* after it is a RLE 24 bit RGB image array\n");
		fprintf(stderr, "   ***\n");
		fprintf(stderr, "Example usage: pssthumb <file> > <output>.ppm\n");
		fprintf(stderr, "               pssthumb <file> | ppmtojpeg > <output>.ppm\n");
		return 0;
	}
#ifdef DEBUG
	else ptr_fname = statfn;
#else
	else {
		fprintf(stderr, "Usage: pssthumb <file>\n");
		return 0;
	}
#endif

	// open file
	fd = open(ptr_fname, O_RDONLY, S_IRWXU|S_IRWXG|S_IRWXO);
	if (fd == -1) {
		fprintf(stderr, "Cannot open file \"%s\"\n", ptr_fname);
		return 1;
	}

	// get file size
	fsize = lseek(fd, 0, SEEK_END);
	// abort if header doesn't fit
	if (fsize < PSS_HEAD) {
		fprintf(stderr, "File is too small (read %d bytes)\n", fsize);
		close(fd);
		return 1;
	}

#ifdef DEBUG
	fprintf(stderr, "Opened %s (%d bytes), reading %d bytes of header.\n", ptr_fname, fsize, PSS_HEAD);
#endif

	// rewind to beginning
	lseek(fd, 0, SEEK_SET);

	// read first 40 bytes of PSS header
	buf = malloc(PSS_HEAD);
	bytes_read = read(fd, buf, PSS_HEAD);

	if (bytes_read < 1) { 
		fprintf(stderr, "Failed to read file.\n"); 
		goto cleanup;
	}
	
	// check if file contains pss signature
	for (unsigned char i=0; i<4; i++)
		if (fsig[i] != buf[i]) {
			fprintf(stderr, "File signature mismatch.\n");
			fprintf(stderr, "Expected %hhx, but got %hhx\n", fsig[i], buf[i]);
			goto cleanup;
		}
	res_w = uswap16(&buf[8]);
	res_h = uswap16(&buf[12]);
	rle_channel_length = res_h * 2;

#ifdef DEBUG
	fprintf(stderr, "File resolution: %dx%d\n", res_w, res_h);
	fprintf(stderr, "RLE information block size: %d bytes\n", rle_channel_length * 3);
#endif

	// bomb protection
	if (res_w > MAX_RES || res_h > MAX_RES) {
		fprintf(stderr, "File is too large, aborting.\n");
		goto cleanup;
	}

	// read rle header into buffer
	rle_header_size = rle_channel_length * 3;

#ifdef DEBUG
	fprintf(stderr, "Allocating %d bytes for RLE header... ", rle_header_size);
#endif
	{
		void *newpointer;
		newpointer = malloc(rle_header_size*sizeof(char));
		if(newpointer == NULL) {
			fprintf(stderr, "malloc() failed - out of memory.\n");
			goto cleanup;
		}
		else {
			free(buf);
			buf = newpointer;
		}
	}

	// rewind after header
	lseek(fd, PSS_HEAD, SEEK_SET);

	// slurp entire rle header to buffer
	bytes_read = read(fd, buf, rle_header_size);
	if (bytes_read == 0) {
		fprintf(stderr, "Failed to read RLE channel of length %d.\n", rle_channel_length);
		goto cleanup;
	}

	{
	unsigned int offset = 0;
	// parse rle compression info
	for (unsigned char ch = 0; ch < 3; ch++) {
		// allocate for compressed rle data for each channel
#ifdef DEBUG
		fprintf(stderr, "Allocating %d bytes\n", rle_channel_length);
#endif 
		compdata_for_channel[ch] = malloc(rle_channel_length * sizeof(int));
		// handle out of memory
		if (!compdata_for_channel[ch]) {
			fprintf(stderr, "Memory allocation failed while decompressing channel %d\n", ch); 
			exit(1); 
		}

		for (unsigned int i = 0; i < rle_channel_length; i+=2) {
			// running on rle i
			char * ptr = &buf[i+offset];
			unsigned short raw = *((unsigned short *)ptr);
			unsigned short swap = (raw>>8) | (raw<<8);
			// compdata_for_channel[ch][i] = ucomb16(&buf[i+offset]);
			compdata_for_channel[ch][i] = swap;
#ifdef DEBUG
			unsigned char a = buf[i+offset];
			unsigned char b = buf[i+offset+1];
			fprintf(stderr, "i: %04u/%u: read %03hu bytes\t", i, rle_channel_length, compdata_for_channel[ch][i]);
			fprintf(stderr, "%02hhx %02hhx ", a, b);
			fprintf(stderr, "\n");
#endif
			rle_total_encoded_size += compdata_for_channel[ch][i];
			rle_channel_size[ch]   += compdata_for_channel[ch][i];

		}
		offset+=rle_channel_length;
	}
	}

#ifdef DEBUG
	fprintf(stderr, "Size of encoded data : %d\n", rle_total_encoded_size);
	fprintf(stderr, "r: %d g: %d b: %d (bytes)\n", rle_channel_size[0], rle_channel_size[1], rle_channel_size[2]); 
	fprintf(stderr, "RLE data starts at %d bytes.\n", PSS_HEAD + rle_header_size);
#endif

	// load RLE compressed image into buffer
	comp_img = malloc(rle_total_encoded_size);
	bytes_read = read(fd, comp_img, rle_total_encoded_size);

#ifdef DEBUG
	fprintf(stderr, "Read %d bytes of encoded data.\n", bytes_read);
#endif

#ifdef DEBUG_VERBOSE
{
	fprintf(stderr, "Encoded array:\n");
	unsigned char width = 8;
	for (unsigned int i = 0; i < rle_total_encoded_size; i+=width){
		for (unsigned int j = 0; j < width; j++){
			if (comp_img[i+j] != 0)
				fprintf(stderr, "0x%02hhx ", comp_img[i+j]);
			else {
				fprintf(stderr, "%s0x%02hhx%s ", RED, comp_img[i+j], CRESET);
			}
		}
		fprintf(stderr, "\n");
	}
	fprintf(stderr, "----------\n");
}
#endif

	// setup containers
	u_init(&image[0]);
	u_init(&image[1]);
	u_init(&image[2]);

	// cross-channel counters
	unsigned int offset = 0;
	unsigned int offset_local = 0;

	unsigned int repeat_c = 0;
	unsigned int out_channel_len[3] = {0, 0, 0};

	// loop over encoded array
	for (unsigned char ch = 0; ch < 3; ch++) {
		for (offset_local = 0; offset_local < rle_channel_size[ch]; offset_local+=2) {
#ifdef DEBUG_VERBOSE
			fprintf(stderr, "comp_img[%d]\t", offset+offset_local);
#endif
			// unsigned channel value
			unsigned char pattern = comp_img[offset+offset_local+1];
			// signed parse rle value
			char repeat = comp_img[offset+offset_local];

			if (repeat == 0) {
#ifdef DEBUG_VERBOSE
				fprintf(stderr, "%03d%s x 1%s\n", pattern, RED, CRESET);
#endif
				repeat_c++;
				u_append( image[ch], pattern);
			}
			else if (repeat >= -127 && repeat <= -1) {
				repeat = repeat * -1 + 1;
				repeat_c += repeat;
#ifdef DEBUG_VERBOSE
				fprintf(stderr, "%03d x %d\n", pattern, repeat);
#endif
				for (char r = 0; r < repeat; r++)
					u_append( image[ch], pattern);
			}
			else if (repeat > 0 && repeat < 127) {
				fprintf(stderr, "%sRLE unpacking: this value (%d) should not be used%s\n", RED, repeat, CRESET);
				goto cleanup;
			}
			else {
				fprintf(stderr, "RLE unpacking: unexpected value %d encountered, exiting...\n", repeat);
				goto cleanup;
			}
		}
#ifdef DEBUG_VERBOSE
		fprintf(stderr, "--- End of channel %d (%d) ---\n", ch, repeat_c);
#endif
		offset += offset_local;
		out_channel_len[ch] = repeat_c;
		repeat_c = 0;
	}

	// Check if number of decoded pixels in channels diverge
	if (out_channel_len[0] != out_channel_len[1] || out_channel_len[1] != out_channel_len[2] || out_channel_len[0] != out_channel_len [2]) {
#ifdef DEBUG
		unsigned int real_len = res_w * res_h;
		fprintf(stderr, "%s%d\t%d\t%d%s\n", RED, out_channel_len[0], out_channel_len[1], out_channel_len[2], CRESET);
		fprintf(stderr, "Real: %d, calculated: %d\n", real_len * 3, out_channel_len[0] + out_channel_len[1] + out_channel_len[2] );
#endif
		goto cleanup;
	}
 	// write data to PPM
 	fprintf(stdout, "P3\n");
	fprintf(stdout, "%d %d\n", res_w, res_h);
	fprintf(stdout, "255\n");
	unsigned long pixel_count = res_h * res_w;
	for (unsigned long i = 0; i < pixel_count; ++i) {
			fprintf(stdout, "%d %d %d\n", u_getat(image[0], i),
				u_getat(image[1], i), u_getat(image[2], i) );
	}
	free(comp_img);
	free(buf);
	close(fd);
	return 0;

cleanup:
	close(fd);
	print_placeholder();
	return 1;
}

// read pointer as little endian uint_16t
unsigned short ucomb16(char* ptr) {
	unsigned short raw = *((unsigned short *)ptr);
	return raw;
}

// read pointer as big endian uint_16t
unsigned short uswap16(char* ptr) {
	unsigned short raw = (ptr[0] << 8) | ptr[1];
	unsigned short swap = (raw>>8) | (raw<<8);
	return swap;
}

void u_init(ch_buf** arr_ptr) {
	ch_buf *container;
	container = (ch_buf*)malloc(sizeof(ch_buf));
	if(!container) { 
		fprintf(stderr, "Memory Allocation Failed\n"); 
		exit(1); 
	} 
  
	container->size = 0; 
	container->capacity = INITIAL_SIZE; 
	container->array = (unsigned char *)malloc(INITIAL_SIZE * sizeof(unsigned char)); 
	if (!container->array){ 
		fprintf(stderr, "Memory Allocation Failed\n"); 
		exit(1); 
	} 
  
    *arr_ptr = container; 
}

void u_print(ch_buf* container) 
{ 
    fprintf(stderr, "Array elements: \n"); 
    for (unsigned int i = 0; i < container->size; i++) { 
        fprintf(stderr, "%d ", container->array[i]); 
    } 
    fprintf(stderr, "Size: %u, Capacity: %u\n", 
			container->size, container->capacity); 
} 

int u_getat(ch_buf* container, unsigned int index) 
{ 
    if(index >= container->size) { 
        fprintf(stderr, "Index %d is out of bounds\n", index); 
        return -1; 
    } 
    return container->array[index]; 
} 

void u_append(ch_buf* container, unsigned char value) 
{ 
	if (container->size == container->capacity) { 
		unsigned char *temp = container->array; 
		container->capacity <<= 1; 
		container->array = realloc(container->array, container->capacity * sizeof(unsigned char)); 
		if(!container->array) { 
			fprintf(stderr, "Out of memory\n"); 
			container->array = temp; 
			return; 
		} 
	} 
	container->array[container->size++] = value; 
}

void print_placeholder(){
	int offset = 0;
 	fprintf(stdout, "P3\n");
	fprintf(stdout, "%d %d\n", PLACEHOLDER_WIDTH, PLACEHOLDER_HEIGHT);
	fprintf(stdout, "255\n");
	for (unsigned int i = 0; i < PLACEHOLDER_WIDTH; i++) {
		for (unsigned int i = 0; i < PLACEHOLDER_WIDTH; i++) {
			fprintf(stdout, "%u ", placeholder[offset]);
			fprintf(stdout, "%u ", placeholder[offset]);
			fprintf(stdout, "%u ", placeholder[offset]);
			fprintf(stdout, "\n");
			offset++;
		}
	}
}
