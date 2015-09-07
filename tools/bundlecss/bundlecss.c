/*
 * Copyright (c) 2014 Hypertriton, Inc. <http://hypertriton.com/>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Generate headers for bundling Agar stylesheets into executables or libraries.
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static void
printusage(void)
{
	fprintf(stderr, "Usage: bundlecss [-abv] [-n name] [-o outfile] "
	                "[infile]\n");
}

int
main(int argc, char *argv[])
{
	const char *outfile, *cssName = "myCSS", *infile;
	extern char *optarg;
	extern int optind;
	int c;
	FILE *f, *fin;
	long i, sizeIn, sizeOut, offs, inComment = 0;
	int append = 0;
	char ch;

	while ((c = getopt(argc, argv, "?ao:n:")) != -1) {
		switch (c) {
		case 'a':
			append = 1;
			break;
		case 'o':
			outfile = optarg;
			break;
		case 'n':
			cssName = optarg;
			break;
		case '?':
		case 'h':
		default:
			printusage();
			exit(1);
		}
	}
	if (optind == argc) {
		printusage();
		exit(1);
	}
	infile = argv[optind];
	if (outfile == NULL) {
		printusage();
		exit(1);
	}
	if ((fin = fopen(infile, "r")) == NULL) {
		fprintf(stderr, "%s: %s\n", infile, strerror(errno));
		exit(1);
	}
	if ((f = fopen(outfile, append ? "a" : "w")) == NULL) {
		fprintf(stderr, "%s: %s\n", outfile, strerror(errno));
		fclose(fin);
		exit(1);
	}
	
	fseek(fin, 0, SEEK_END);
	sizeIn = ftell(fin);
	fseek(fin, 0, SEEK_SET);

	fprintf(f, "/* File generated by agar bundlecss */\n");
	
	fprintf(f, "const char *%s_Data = \n\t\"", cssName);
	for (i = 0, offs = 0, sizeOut = 0;
	     i < sizeIn;
	     i++) {
		fread(&ch, 1, 1, fin);
		
		if (offs == 0 && ch == '#') {
			inComment = 1;
		} else if (ch == '\n') {
			if (!inComment && offs > 0) {
				fputs("\\n\"\n\t\"", f);
				sizeOut++;
			}
			offs = 0;
			inComment = 0;
			continue;
		}
		if (inComment || (ch == '\t' && offs == 0)) {
			offs++;
			continue;
		}
		if (ch == '"') {
			fputs("\\\"", f);
			sizeOut+=2;
		} else {
			fputc(ch, f);
			sizeOut++;
		}
		offs++;
	}
	fprintf(f, "\";\n\n");
	fprintf(f, "AG_StaticCSS %s = {\n", cssName);
	fprintf(f, "\t\"%s\",\n", cssName);
	fprintf(f, "\t%ld,\n", sizeOut);
	fprintf(f, "\t&%s_Data,\n", cssName);
	fprintf(f, "\tNULL\n};\n");

	fclose(f);
	fclose(fin);
	return (0);
}
