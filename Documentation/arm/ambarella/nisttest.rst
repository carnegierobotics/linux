.. SPDX-License-Identifier: GPL-2.0

======================================================
Ambarella nisttest
======================================================

Table of Contents
=================
I - Reference
II - Introduction
III - Randomness verification
IV - How to Get Started
V - Number Generator
VI - Test flow
VII - Appendix: reference code

I - Reference
-------------
#. Documentation/hw_random.txt
#. NIST Statistical Test Suite
#. NIST user guide: A Statistical Test Suite for Random and Pseudorandom Number Generators
   for Cryptographic Applications

PS:NIST Statistical Test Suite and NIST user guide can be downloaded from
https://csrc.nist.gov/Projects/Random-Bit-Generation/Documentation-and-Software

II - Introduction
-----------------
RNG stands for Random Number Generator, and the output is used for "encryption key" generation.
The linux driver provides /dev/hwrng character device through hw_random framework.To check if
RNG output is random enough, this page provides the information about how to verify the randomness.
It contains randomness verification, number generation and test result on different platform.

III - Randomness verification
-----------------------------
Statistic strategy is used to verify if the a bit stream is random enough.
NIST SP-800-22 test suite (15 tests included) is used for verification. Ths 15 tests is as follows:

#. The Frequency (Monobit) Test,
#. Frequency Test within a Block,
#. The Runs Test,
#. Tests for the Longest-Run-of-Ones in a Block,
#. The Binary Matrix Rank Test,
#. The Discrete Fourier Transform (Spectral) Test,
#. The Non-overlapping Template Matching Test,
#. The Overlapping Template Matching Test,
#. Maurer's "Universal Statistical" Test,
#. The Linear Complexity Test,
#. The Serial Test,
#. The Approximate Entropy Test,
#. The Cumulative Sums (Cusums) Test,
#. The Random Excursions Test, and
#. The Random Excursions Variant Test.

Please refer to NIST user guide for more details about each statistics test and user guidance.

IV - How to Get Started
-----------------------
To setup a copy of the NIST test code on a workstation, follow the instructions below.

#. Copy the sts.tar file into the root directory. Use the instruction, tar -xvf sts.tar,
   to unbundle the source code.
#. Six subdirectories and one file should have been created. The subdirectories are:
   data/, experiments/, include/, obj/, src/ and templates/. The file is makefile.
#. The data/ subdirectory is reserved for pre-existing RNG data files that are under
   investigation. Currently, two formats are supported, i.e., data files consisting of
   ASCII zeroes and ones, and binary data files.
#. The experiments/ subdirectory will be the repository of the empirical results for the
   statistical tests. Several subdirectories should be contained in it. These include
   AlgorithmTesting/, BBS/, CCG/, G-SHA1/, LCG/, MODEXP/, MS/, QCG1/, QCG2/, and XOR/.
   All but the first of these subdirectories is meant to store the results for the
   corresponding PRNG. The AlgorithmTesting/ subdirectory is the default subdirectory for
   empirical results corresponding to RNG data stored in the data/ subdirectory.
#. The include/ subdirectory contains the header files for the statistical tests, pseudorandom
   number generators, and associated routines.
#. The obj/ subdirectory contains the object files corresponding to the statistical tests,
   pseudo random number generators and other associated routines.
#. The src/ subdirectory contains the source codes for each of the statistical tests.
#. The templates/ subdirectory contains a series of non-periodic templates for varying
   block sizes that are utilized by the NonOverlapping Templates statistical test.
#. User prescribed modifications may be introduced in several files.
#. Edit the makefile. Modify the following lines:
   *  CC (your ANSI C compiler)
   *  ROOTDIR (the root directory that was prescribed earlier in the process, e.g., rng/)
#. Now execute makefile. An executable file named assess should appear in the project directory.
#. The data may now be evaluated. Type the following: assess <sequenceLength>, e.g.,
   assess 1000000.

Follow the menu prompts.

V Number Generator
------------------
To run all tests in test suite, at least 1,000,000 length bit stream is needed. Since
RNG generates 4 pieces of data(32b) at each time, we need to loop 250,000 times to
collect 1,000,000 bit.

#. Write an app to read /dev/hwrng to get test data. At least to read 1000000*32*10
   bits(4*10^7). We can read /dev/hwrng 16bytes(128bits) every time. Read 2,500,000
   times. Also we can get more data to test.
#. Extract 32 bits streams form the read data.

   ::

    Trial   Loop      RNG output   Bit Stream 31      Bit Stream 30     ... Bit Stream 0
    0        0        Data0        bit31(n=0)         bit30(n=0)        ... bit0(n=0)
                      Data1        bit31(n=1)         bit30(n=1)        ... bit0(n=1)
                      Data2        bit31(n=2)         bit30(n=2)        ... bit0(n=2)
                      Data3        bit31(n=3)         bit30(n=3)        ... bit0(n=3)
             1        Data0        bit31(n=4)         bit30(n=4)        ... bit0(n=4)
                      Data1        bit31(n=5)         bit30(n=5)        ... bit0(n=5)
                      Data2        bit31(n=6)         bit30(n=6)        ... bit0(n=6)
                      Data3        bit31(n=7)         bit30(n=7)        ... bit0(n=7)
             ...      ...          ...                ...               ... ...
             249,999  Data0        bit31(n=999,996)   bit30(n=999,996)  ... bit0(n=999,996)
                      Data1        bit31(n=999,997)   bit30(n=999,997)  ... bit0(n=999,997)
                      Data2        bit31(n=999,998)   bit30(n=999,998)  ... bit0(n=999,998)
                      Data3        bit31(n=999,999)   bit30(n=999,999)  ... bit0(n=999,999)
    1        0        Data0        bit31(n=0)         bit30(n=0)        ... bit0(n=0)
                      Data1        bit31(n=1)         bit30(n=1)        ... bit0(n=1)
                      Data2        bit31(n=2)         bit30(n=2)        ... bit0(n=2)
                      Data3        bit31(n=3)         bit30(n=3)        ... bit0(n=3)
             1        Data0        bit31(n=4)         bit30(n=4)        ... bit0(n=4)
                      Data1        bit31(n=5)         bit30(n=5)        ... bit0(n=5)
                      Data2        bit31(n=6)         bit30(n=6)        ... bit0(n=6)
                      Data3        bit31(n=7)         bit30(n=7)        ... bit0(n=7)
             ...      ...          ...                ...               ... ...
             249,999  Data0        bit31(n=999,996)   bit30(n=999,996)  ... bit0(n=999,996)
                      Data1        bit31(n=999,997)   bit30(n=999,997)  ... bit0(n=999,997)
                      Data2        bit31(n=999,998)   bit30(n=999,998)  ... bit0(n=999,998)
                      Data3        bit31(n=999,999)   bit30(n=999,999)  ... bit0(n=999,999)
    ...      ...      ...          ...                ...               ... ...
    9        0        Data0        bit31(n=0)         bit30(n=0)        ... bit0(n=0)
                      Data1        bit31(n=1)         bit30(n=1)        ... bit0(n=1)
                      Data2        bit31(n=2)         bit30(n=2)        ... bit0(n=2)
                      Data3        bit31(n=3)         bit30(n=3)        ... bit0(n=3)
             1        Data0        bit31(n=4)         bit30(n=4)        ... bit0(n=4)
                      Data1        bit31(n=5)         bit30(n=5)        ... bit0(n=5)
                      Data2        bit31(n=6)         bit30(n=6)        ... bit0(n=6)
                      Data3        bit31(n=7)         bit30(n=7)        ... bit0(n=7)
             ...      ...          ...                ...               ... ...
             249,999  Data0        bit31(n=999,996)   bit30(n=999,996)  ... bit0(n=999,996)
                      Data1        bit31(n=999,997)   bit30(n=999,997)  ... bit0(n=999,997)
                      Data2        bit31(n=999,998)   bit30(n=999,998)  ... bit0(n=999,998)
                      Data3        bit31(n=999,999)   bit30(n=999,999)  ... bit0(n=999,999)

Besides, the randomness across each 32-bit data is also investigated.
The bit stream is re-mapped from each specific bit stream (up to 1,000,000 bits).

::

  Trial   Loop   RNG output   Bit stream
  0       0      Data0        bit31
                              bit30
                              ...
                              bit0
                 Data1        bit31
                 ...          ...
                 Data3        bit31
          1      Data0        bit31
                 ...          ...
          2      Data0        bit31
                 ...          ...
  ...     ...    ...          ...
  n       m      ...         ...

PS: Collcting data and extracting data programs can be find in the appendix at the last of the text.

VI Test flow
------------

#. Generate random number with 10 trials.
#. Save stream files to SD card and user extract app to extract 32 bit streams.
#. Move 32 bit stream files and the raw bit stream into test suite.
#. Exccute NIST test suite. Enter sts-2.1.2 folder and type "./assess 1000000" in the console
   #. input 0 then input the file name
   #. select 1 to test each statistical test
   #. enter 0 to continue
   #. input how many bitstreams? We input 10.
   #. input file format, then the data will be processing
   #. Note that to automate the process, source code is modified and hard code all options.
#. Analyze the resuls

The reports are available on sts-2.1.2/experiments/AlgorithmTesting/finalAnalysisReport.txt.
The results are represented via a table with p rows and q columns. The number of rows, p,
corresponds to the number of statistical tests applied. The number of columns, q = 13, are
distributed as follows: columns 1-10 correspond to the frequency of P-values, column 11 is
the P-value that arises via the application of a chi-square test, column 12 is the proportion
of binary sequences that passed, and the 13 column is the corresponding statistical test.

VII Appendix: reference code
----------------------------

reference code(readrng.c)

::

  #include <stdio.h>
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <fcntl.h>
  #include <unistd.h>

  #define RNG_DEVICE "/dev/hwrng"

  int main(int argc, char **argv)
  {
  	int rng_fd;
  	int sfd;
  	int count;
  	int wc;
  	unsigned long total = 0;
  	char buf[128];

  	if (argc != 2) {
  		printf("%s filename\n", argv[0]);
  		return -1;
  	}
  	rng_fd = open(RNG_DEVICE, O_RDONLY);
  	if(rng_fd < 0) {
  		printf("can not open %s\n", RNG_DEVICE);
  		return -1;
  	}

  	sfd = open(argv[1], O_RDWR);
  	if(sfd < 0) {
  		close(rng_fd);
  		printf("can not open %s\n", sfd);
  		return -1;
  	}

  	while (1)
  	{
  		count = read(rng_fd, buf, 16);
  		if(count > 0) {
  			wc = write(sfd, buf, count);
  			if(wc!=count)
  				printf("write wrong happen\n");
  		}
  		total += count;
  		if(total > 40000000)
  			break;
  	}

  	close(sfd);
  	close(rng_fd);

  	return 0;
  }

#. cross compile readrng.c to generate readrng

   ::

    aarch64-linux-gnu-gcc -o readrng readrng.c

#. push readrng on sd card
#. inser sd card
#. cd /sdcard, run readrng which would be take long time to collct enough data readrng rngdat

Note that readrng's data format is in binary.
---------------------------------------------

reference code(extract_data.c)

::

 #include <stdio.h>
 #include <sys/types.h>
 #include <sys/stat.h>
 #include <fcntl.h>
 #include <unistd.h>
 #include <stdlib.h>

 int main(int argc, char **argv)
 {
 	int rng_fd;
 	int wfd[32];
 	int count;
 	unsigned char buf[1024];
 	unsigned char tmp[32];
 	char wname[32];
 	unsigned int wdata;

 	unsigned i;
 	unsigned long j;

 	if (argc != 2) {
 		printf("%s filename\n", argv[0]);
 		return -1;
 	}

 	rng_fd = open(argv[1], O_RDONLY);
 	if(rng_fd < 0) {
 		printf("can not open %s\n", argv[1]);
 		return -1;
 	}

 	for(i=0;i<32;i++) {
 		sprintf(wname, "%s_%d_bits", argv[1], i);
 		wfd[i] = open(wname, O_RDWR | O_CREAT |O_TRUNC);
 		if(wfd[i] < 0) {
 			printf("can not open/create %s\n", wname);
 			exit(-1);
 		}
 	}

 	for(j=0;;j++)
 	{
 		count = read(rng_fd, buf, 4);
 		if(count != 4) {
 			printf("read error, read %ld\n", 4*j);
 			break;
 		}
 		wdata = buf[0] | (buf[1]<<8) | (buf[2] << 16) | (buf[3] <<24);
 		for(i=0; i<32; i++) {
 			if (write(wfd[i], (wdata & 0x01) ? "1" : "0", 1) != 1) {
 				printf("wrtie error\n");
 				exit(-1);
 			}
 			wdata >>= 1;
 		}
 	}
 	printf("4*j = %ld\n",j*4);

 	close(rng_fd);
 	for(i=0;i<32;i++) {
 		close(wfd[i]);
 	}

 	return 0;
 }

#. compile extract_data.c to generate extract_data

   ::

      gcc -o extract_data extract_data.c

#. run extract_data to extract every bit streams, rngdat_x_bits(x means which bit streams(form 0 to 31)) extract_data rngdat Note that rngdat_x_bits's data format are in ASCII.
