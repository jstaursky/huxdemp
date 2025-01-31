/// ## The code
///
/// First, the includes.
///
/// * err.h: A little-known header, originating in BSD, that contains various
///   utility functions for printing error/warning messages (see err(3)).
///
/// * ctype.h: Included for isprint(3), which checks whether a character is a
///   control character or not.
///
/// * stdbool.h: Included for the `true'/`false' values.
///
/// * stdint.h: Included for `size_t', which is an unsigned integer type with a
///   bitsize equivalent to the target machine's pointer size.  For example: on a
///   32-bit machine, size_t == uint32_t, while on a 64-bit machine, size_t ==
///   uint64_t.
///
/// * string.h: Used for `strcmp'/`strncmp', the sole string function used in this
///   program.
///
/// * stdlib.h: used for `getenv'.
///
/// * unistd.h: used for `popen`/`pclose`, `isatty' and the `STDOUT_FILENO'
///   constant.
///
/// `size_t' used throughout instead of `int' where possible because it's faster to
/// use an unsigned integer, and it's almost always faster to use an integer with a
/// bit size equivalent to the CPU's word size(?).
///
#include <err.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>  /* for str(n)cmp */
#include <stdlib.h>  /* for getenv */
#include <unistd.h>  /* for isatty, STDOUT_FILENO */

/// Some utility macros.
///
/// * ARRAY_LEN returns the length of a statically-allocated array.
/// * UNUSED marks a function parameter as "unused" so we don't get warnings for it.
/// * MAX and MIN do what you'd expect.
///
#define ARRAY_LEN(A) (sizeof(A) / sizeof((A)[0]))
#define UNUSED(X)    ((void)(X))
#define MAX(V, H)    ((V) > (H) ? (V) : (H))
#define MIN(V, H)    ((V) < (H) ? (V) : (H))

/// People these days should stop pretending that C's `char' is really a character.
/// Plus byte_t is more readable.
///
typedef unsigned char byte_t;

/// The various builtin display functions. There's a list of display functions to be
/// used in the options struct, and each of them are called in huxdemp on the input.
/// The user can reorder this list via an option; this is how column reordering is
/// implemented.
///
enum Column {
	CO_Offset,
	CO_Bytes,
	CO_BytesLeft,
	CO_BytesRight,
	CO_Ascii,
	CO_AsciiLeft,
	CO_AsciiRight,
	CO_Plugin,
};

/// An enum for storing the values of options like -C and -P (i.e., when to use
/// colors and pagers).
///
enum ActionMode {
	AM_Always, AM_Auto, AM_Never
};

/// A single struct that holds all the options for this program.
/// `table' is set by the `-t' flag, `cntrls' is set by the `-c' flag, etc.
///
/// NOTE: _color is not set directly by the user; its value is
/// determined after the user has set the value of the `color'/`pager' field.
///
#define MAX_LINELEN 128

struct {
	char **table;
	_Bool ctrls, utf8;

	enum ActionMode color;
	enum ActionMode pager;

	_Bool _color;

	size_t linelen;
	uint64_t offset;
	uint64_t length;

	enum Column dfuncs[255];
	char dfunc_names[255][255];
	size_t dfuncs_sz;
} options;

/// A table to hold the colors used for all byte values. This will be set later in
/// main() with config().
///
uint8_t styles[256] = {0};

/// Keep track of what UTF8 codepoint we're printing. This allows us to highlight
/// bytes that belong to the same Unicode codepoint.
///
/// This is reset after each file is processed, for obvious reasons.
///
/// { <offset>, <length> }:
/// * Offset: the position of the first byte comprising the encoded UTF8 codepoint.
/// * Length: the number of bytes that make up the UTF8 codepoint.
///
static ssize_t utf8_state[] = { -1, -1 };


/// Now to include other source files.
///
/// We're doing this *here* instead of back when we included other standard files,
/// because we want included C files to be able to reference the globals defined
/// earlier (like the options struct).
///
/// * arg.h: The ubiquitious arg.h you would find in many suckless.org projects.
///   It's used to parse arguments in place of getopt, as I find it slightly easier
///   to work with. However, it does things the strictly POSIX way and refuses to
///   parse flags after a non-flag argument was recieved.
///
/// * builtin.c: Some builtin plugins. Basically, embedded Lua code that's evaluated
///   at runtime.
///
/// * lua.c: Lua utility functions. Also includes the lua headers (lauxlib.h,
///   lua.h, etc).
///
/// * tables.c: Various 'tables' of strings that are used to display characters.
///   E.g. "·" for 0x12, "A" for 0x41, etc.
///
/// * range.c: Utility funcs for parsing ranges (e.g., "0-3", "1,2,3-4,5", etc).
///   Used for parsing $HUXD_COLORS in config().
///
/// * utf8.c: An extremely simple UTF8 library proudly stolen from the termbox[1]
///   source code.
///
/// The C files are directly included into main.c (instead of being compiled into
/// their own object files) because I'm too lazy to add a few more lines to the
/// Makefile.
///
/// [1]: https://github.com/nsf/termbox
///
#include "arg.h"
#include "builtin.c"
#include "lua.c"
#include "tables.c"
#include "utf8.c"
#include "range.c"

/// This function is run before each item of the byte column is printed to update
/// `utf8_state'.  It only actually updates `utf8_state' if either the first field
/// is -1, or we've passed the codepoint utf8_state keeps track of.
///
static inline void
_utf8state(ssize_t offset, byte_t _char)
{
	if (utf8_state[0] == -1 || utf8_state[0]+utf8_state[1] < offset) {
		utf8_state[0] = offset;
		utf8_state[1] = utf8_sequence_length(_char) - 1;
	}
}

/// A utility function to determine how we should display a character in the ASCII
/// column.
///
/// * If the byte is a control character and the user enabled `options.ctrl', we
///   use some fancy Unicode graphics to display it (e.g. ␀ for 0x0).
///
/// * If the user has specified a table/style to use, use that byte's entry from
///   that table (if the entry exists).
///
/// * Otherwise, return the byte itself (unless it's unprintable, in which case use
///   a period).
///
/// We're returning a char* instead of just a byte_t because many of the
/// characters in tables.c are actually strings (because they are Unicode, not
/// ASCII, and cannot fit within a byte).
///
static inline char *
_format_char(byte_t b)
{
	static char chbuf[2] = {0};

	if (options.ctrls && t_cntrls[b])
		return t_cntrls[b];

	if (options.table && options.table[b])
		return options.table[b];

	chbuf[0] = isprint(b) ? b : '.';
	return (char *)&chbuf;
}

/// Now the juicy bit -- the functions which actually do the work of displaying the
/// input.
///
/// ---
///
/// Display the byte offset in hexadecimal, padding it with up to four spaces to
/// the left.
///
/// * `\x1b[37m': Display the text in light grey.
/// * `\x1b[m':   Reset the text's color.
///
///   40    72 69 20 61 73 20 68 c3  ab 20 68 61 64 20 73 70    │ri as h·· had sp│
///   50    6f 6b 65 6e 20 62 65 66  6f 72 65 20 69 6e 20 54    │oken before in T│
///   60    c3 af 72 69 6f 6e 2e 20  7f 0a 0a                   │··rion. «__│
///   ^~
///
static void
display_offset(size_t offset, _Bool use_color, FILE *out)
{
	if (use_color) {
		fprintf(out, "\x1b[37m%4zx\x1b[m", offset);
	} else {
		fprintf(out, "%08zx", offset);
	}
}

/// Display the byte column.
///
/// * Update the UTF8 state.
///
/// * If we're halfway through, print a space. This splits the byte column into two
///   columns.
///
/// * Print an escape sequence to set the background of the hex digits depending on
///   whether it belongs to an encoded utf8 codepoint.
///
/// * Print the byte's hex digits, using the styling specified in tables.c.
///
/// * If we're "out" of the last utf8 codepoint sequence, reset the background
///   color; otherwise, only reset the foreground color and remove the bold
///   formatting (if any).
///
///   40    72 69 20 61 73 20 68 c3  ab 20 68 61 64 20 73 70    │ri as h·· had sp│
///   50    6f 6b 65 6e 20 62 65 66  6f 72 65 20 69 6e 20 54    │oken before in T│
///   60    c3 af 72 69 6f 6e 2e 20  7f 0a 0a                   │··rion. «__│
///         ^~~~~~~~~~~~~~~~~~~~~~~  ^~~~~~~~~~~~~~~~~~~~~~~
///
/// * Reset the terminal colors.
///
/// * Print some padding:
///
///   40    72 69 20 61 73 20 68 c3  ab 20 68 61 64 20 73 70    │ri as h·· had sp│
///   50    6f 6b 65 6e 20 62 65 66  6f 72 65 20 69 6e 20 54    │oken before in T│
///   60    c3 af 72 69 6f 6e 2e 20  7f 0a 0a                   │··rion. «__│
///                                           ^~~~~~~~~~~~~~
///
///   The amount of padding is calculated like so:
///
///   LINELEN - sz   The number of hex digits that weren't printed on this line.
///           *  3   Each hex digit takes up three columns.
///           +  1   Only if the space that divides the two hex digit columns wasn't
///                  printed.
///
static void
_display_byte(byte_t byte, size_t off, _Bool use_color, FILE *out)
{
	if (use_color) {
		_utf8state((ssize_t)off, byte);

		size_t bg = 0, fg = styles[byte];
		if (options.utf8 && utf8_state[1] > 0)
			bg = 100, fg = 97;

		fprintf(out, "\x1b[%zum\x1b[38;5;%zum%02hx", bg, fg, byte);

		if (utf8_state[0] + utf8_state[1] <= (ssize_t)off)
			fprintf(out, "\x1b[m ");
		else
			fprintf(out, "\x1b[37m\x1b[22m ");
	} else {
		fprintf(out, "%02hx ", byte);
	}
}

static void
display_bytes(byte_t *buf, size_t buf_sz, size_t offset, _Bool use_color, FILE *out)
{
	for (size_t off = offset, i = 0; i < buf_sz; ++i, ++off) {
		if (i == (options.linelen / 2))
			fprintf(out, " ");

		_display_byte(buf[i], off, use_color, out);
	}

	if (use_color) {
		fprintf(out, "\x1b[m");
	}

	fprintf(out, "%*s", (int)(options.linelen - buf_sz) * 3, "");
	if (buf_sz <= (options.linelen / 2))
		fprintf(out, " ");
}

static void
display_bytes_left(byte_t *buf, size_t buf_sz, size_t offset, _Bool use_color, FILE *out)
{
	for (
		size_t off = offset, i = 0;
		i < buf_sz && i < (options.linelen / 2);
		++i, ++off
	) _display_byte(buf[i], off, use_color, out);

	if (use_color) {
		fprintf(out, "\x1b[m");
	}

	if ((options.linelen / 2) > buf_sz) {
		fprintf(out, "%*s", (int)((options.linelen / 2) - buf_sz) * 3, "");
	}
}

static void
display_bytes_right(byte_t *buf, size_t buf_sz, size_t offset, _Bool use_color, FILE *out)
{
	for (size_t off = offset, i = options.linelen / 2; i < buf_sz; ++i, ++off)
		_display_byte(buf[i], off, use_color, out);

	if (use_color) {
		fprintf(out, "\x1b[m");
	}

	fprintf(out, "%*s", (int)(options.linelen - buf_sz) * 3, "");
}

/// Print the usual four spaces, a nice Unicode vertical line-drawing glyph, and
/// each character for the ASCII column styled with the escape codes specified in
/// tables.c. Finally, add space padding.
///
///   40    72 69 20 61 73 20 68 c3  ab 20 68 61 64 20 73 70    │ri as h·· had sp│
///   50    6f 6b 65 6e 20 62 65 66  6f 72 65 20 69 6e 20 54    │oken before in T│
///   60    c3 af 72 69 6f 6e 2e 20  7f 0a 0a                   │··rion. «__│
///                                                             ^~~~~~~~~~~~~~~~~~
///
static void
display_ascii(byte_t *buf, size_t buf_sz, size_t linelen, _Bool use_color, FILE *out)
{
	fprintf(out, "%s", use_color ? "│" : "|");
	for (size_t i = 0; i < buf_sz; ++i) {
		if (use_color) {
			fprintf(out, "\x1b[38;5;%hdm%s\x1b[m",
				styles[buf[i]], _format_char(buf[i]));
		} else {
			fprintf(out, "%s", _format_char(buf[i]));
		}
	}
	fprintf(out, "%*s", (int)(linelen - buf_sz), "");
	fprintf(out, "%s", use_color ? "│" : "|");
}

/// A utility func to start less, and make our stdout point to less's stdin. This allows
/// output to be piped through less automatically (kinda like `git log`).
///
/// * The "-F" flag is given to make less quit immediately if the output can be
///   displayed without scrolling.
///
/// * The FILE* value is returned so that it can be passed to pclose() later on.
///
static FILE *
pager(enum ActionMode mode)
{
	char *command = NULL;
	switch (mode) {
	break; case AM_Never:  command = NULL;
	break; case AM_Always: command = "less -R";
	break; case AM_Auto:
		if (isatty(STDOUT_FILENO)) {
			command = "less -F -R";
		}
	}

	if (command == NULL) {
		return stdout;
	}

	FILE *pager = popen(command, "w");
	if (pager == NULL) {
		warn("Couldn't execute pager (use '-P never' to disable)");
		return stdout;
	}

	return pager;
}

/// The "main main" function that's called from main() after arguments are parsed.
/// We just take a path, open it (if it's not stdin), and call `display*()' after
/// reading LINELEN bytes.
///
static void
huxdemp(char *path, FILE *out)
{
	/* Reset UTF8 state for each file. */
	utf8_state[0] = utf8_state[1] = -1;

	FILE *fp = !strcmp(path, "-") ? stdin : fopen(path, "r");

	if (fp == NULL) {
		warn("\"%s\"", path);
		goto cleanup;
	}

	byte_t buf[MAX_LINELEN];
	size_t offset = 0;

	/// Determine the offset to start at. By default it's zero, but if the -s option is
	/// passed we try to seek forward in the stream to that offset.
	///
	/// TODO: check that the provided offset isn't negative, etc
	///
	if (options.offset != 0) {
		int r = fseek(fp, (long)options.offset, SEEK_SET);
		if (r == -1) {
			warn("\"%s\": Couldn't seek to offset %ld",
				path, options.offset);
			goto cleanup;
		} else {
			/* Use ftell's return value, so that if the
			 * user-provided value is beyond the stream's bounds, we
			 * won't be setting offset to an erroneous value. */
			offset = ftell(fp);
		}
	}
	/// start_offset is used to determine when to stop reading from the file when the -n
	/// option is passed.
	size_t start_offset = offset;

	/// Main loop. Erase buffer, read as much bytes as we can (and stop if we can't or
	/// shouldn't read any more), and close the stream afterwards.
	for (size_t r = 0; ;) {
		memset(buf, 0x0, sizeof(buf));

		size_t max_read = options.linelen;
		if (options.length > 0) {
			size_t bytes_left = options.length - (offset - start_offset);
			max_read = MIN(options.linelen, bytes_left);
		}

		if (!(r = fread(buf, 1, max_read, fp)))
			break;

		for (size_t i = 0; i < options.dfuncs_sz; ++i) {
			switch (options.dfuncs[i]) {
			break; case CO_Offset:
				display_offset(offset, options._color, out);
			break; case CO_Bytes:
				display_bytes(buf, r, offset, options._color, out);
			break; case CO_BytesLeft:
				display_bytes_left(buf, r, offset, options._color, out);
			break; case CO_BytesRight:
				display_bytes_right(buf, r, offset, options._color, out);
			break; case CO_Ascii:
				display_ascii(buf, r, options.linelen,
					options._color, out);
			break; case CO_AsciiLeft:
				display_ascii(buf, MIN(r, options.linelen / 2),
					options.linelen / 2, options._color, out);
			break; case CO_AsciiRight:
				;
				size_t linelenhalf = options.linelen / 2;
				if (r > linelenhalf) {
					display_ascii(&buf[linelenhalf], r - linelenhalf,
						linelenhalf, options._color, out);
				}
			break; case CO_Plugin:
				call_plugin(i, buf, r, offset, out);
			}

			fprintf(out, "    ");
		}
		fprintf(out, "\n");

		offset += r;
	}

cleanup:
	if (fp != NULL && fp != stdin)
		fclose(fp);

	fprintf(out, "\n");
}

/// Check whether we should use colors, based on whether the user selected `auto',
/// `always', or `never'.
///
/// If `auto':
/// * If stdout is not a terminal, no colors.
/// * If the $NO_COLOR environment variable is defined, then no colors.
/// * If $TERM is NULL or is set to "dumb", then no colors.
/// * Otherwise, enable colors.
///
static _Bool
_decide_color(void)
{
	if (options.color == AM_Always)
		return true;
	if (options.color == AM_Never)
		return false;

	if (!isatty(STDOUT_FILENO))
		return false;

	char *env_NOCOLOR = getenv("NO_COLOR");
	char *env_TERM = getenv("TERM");

	if (env_NOCOLOR)
		return false;

	if (!env_TERM || !strcmp(env_TERM, "dumb"))
		return false;

	return true;
}

/// Parse a config string and apply to the `styles` table in tables.c.
///
static void
config(const char *config_str)
{
	// Copy the (constant) argument to our own buffer, since we're going to modify it
	// with strsep when splitting it.
	char *conf_buf = malloc(strlen(config_str) + 1);
	if (conf_buf == NULL) {
		warn("Please hit Alt+F4 a few times");
		return;
	}
	strcpy(conf_buf, config_str);

	// Now the actual parsing.
	//
	// First, split each statement along the ";"
	//
	for (char *ptr = conf_buf; ptr;) {
		char *statement = strsep(&ptr, ";");
		if (*statement == '\0') continue;
		// Now, split along the "=".
		char *eql = strchr(statement, '=');
		if (eql == NULL) {
			warnx("Couldn't parse config: '%s' is malformed", statement);
			return;
		}
		*eql = '\0';
		char *lhand = statement;
		char *rhand = eql + 1;

		// Pre-defined strings are expanded to pre-defined ranges to make configuring this
		// lame program slightly less painful.
		char *range = lhand;
		if (!strcmp(lhand, "printable"))   range = "0x20-0x7E";
		if (!strcmp(lhand, "unprintable")) range = "0x0-0x1F,0x7F";
		if (!strcmp(lhand, "whitespace"))  range = "0x8-0xD,0x20";
		if (!strcmp(lhand, "blackspace"))  range = "0x08,0x7F";
		if (!strcmp(lhand, "nul"))         range = "0x0";
		if (!strcmp(lhand, "del"))         range = "0x7F";

		// Now, expand the range into `range_out`. expand_range() returns the number of
		// items in the range, or -1 if the range was invalid.
		//
		// Examples:
		// 	- "0-1" ⇒ {0,1}
		// 	- "whitespace" ⇒ {8,9,10,11,12,13,32}
		// 	- "nul" ⇒ {0}
		byte_t range_out[256];
		ssize_t range_len = expand_range(range, range_out);
		if (range_len == -1) {
			warnx("Couldn't parse config: %s is not a valid range", range);
			return;
		}

		// Parse the right-hand side of the config statement. We do our own base detection
		// because we don't want "0300" to be parsed as an octal (the only true way to do
		// it is "0o300").
		//
		// FIXME: we don't reject invalid numbers here (but trailing whitespace should be
		// OK).
		size_t base = 10;
		if (!strncmp(rhand, "0o", 2)) base = 8,  rhand += 2;
		if (!strncmp(rhand, "0x", 2)) base = 16, rhand += 2;
		if (!strncmp(rhand, "0b", 2)) base = 2,  rhand += 2;
		size_t rhand_num = strtol(rhand, NULL, base);

		if (rhand_num > 255) {
			warnx("Couldn't parse config: '%ld' is out of range (only 255 colors!)", rhand_num);
			return;
		}

		// Finally, apply the config statement.
		for (size_t i = 0; i < (size_t)range_len; ++i) {
			styles[range_out[i]] = (uint8_t)rhand_num;
		}
	}
}

/// Print a usage string and exit.
///
static _Noreturn void
_usage(char *argv0)
{
	printf("Usage: %s [-hV]\n", argv0);
	printf("       %s [-cu] [-n length] [-s offset] [-l bytes] [-t table]\n", argv0);
	printf("       %*s [-f format] [-C color?] [-P pager?] [FILE]...\n",
		(int)strlen(argv0), "");
	printf("\n");
	printf("Flags:\n");
	printf("    -c  Use Unicode glyphs to display the lower control\n");
	printf("        chars (0 to 31). E.g. ␀ for NUL, ␖ for SYN (0x16), &c\n");
	printf("    -u  Highlight sets of bytes that 'belong' to the same UTF-8\n");
	printf("        encoded Unicode character.\n");
	printf("    -h  Print this help message and exit.\n");
	printf("    -V  Print huxd's version and exit.\n");
	printf("\n");
	printf("Options:\n");
	printf("    -f  Change info columns to display. (default: \"offset,bytes,ascii\")\n");
	printf("        Possible values: `offset', `bytes', `bytes-left', `bytes-right',\n");
	printf("                         `ascii', `ascii-left', `ascii-right'.\n");
	printf("        Using a value not in the above list will make huxd look for a\n");
	printf("        plugin by that name (with a trailing dash and text trimmed off).\n");
	printf("        Example: 'foo' will load plugin foo.lua, as will 'foo-bar'.\n");
	printf("    -l  Number of bytes to be displayed on a line. (default: 16)\n");
	printf("    -n  Maximum number of bytes to be read (can be used with -s flag).\n");
	printf("    -s  Number of bytes to skip from the start of the input. (default: 0)\n");
	printf("    -t  What 'table' or style to use.\n");
	printf("        Possible values: `default', `cp437', or `classic'.\n");
	printf("    -C  When to use fancy terminal formatting.\n");
	printf("        Possible values: `auto', `always', `never'.\n");
	printf("    -P  When to run the output through a less(1).\n");
	printf("        Possible values: `auto', `always', `never'.\n");
	printf("\n");
	printf("Arguments are processed in the same way that cat(1) does: any\n");
	printf("arguments are treated as files and read, a lone \"-\" causes huxd\n");
	printf("to read from standard input, &c.\n");
	printf("\n");
	printf("See the manpage huxd(1) for more documentation.\n");
	exit(0);
}

///
/// main main main
///
int
main(int argc, char *argv[])
{
	// Set some sensible default options.
	//
	// Of course, since `options` is a global variable, it's automatically zeroed out,
	// but we're setting some options to zero anyway here to be explicit.
	//
	options.table = (char **)&t_default;
	options.ctrls = options.utf8 = false;
	options.color = options.pager = AM_Auto;
	options.linelen = 16;
	options.offset = 0;
	options.length = 0;
	options.dfuncs[0] = CO_Offset;
	options.dfuncs[1] = CO_Bytes;
	options.dfuncs[2] = CO_Ascii;
	options.dfuncs_sz = 3;

	// A color config that's evaluated by config() (before $HUXD_COLORS) to set the
	// default colors.
	char *default_colors =
		"printable=15;blackspace=1;nul=8;whitespace=8;128-255=3;1-8=6;11-31=6";

	// Initialize Lua. (We're doing this now, instead of later, because we'll be
	// loading lua files during arg parsing.)
	luau_init(&L);
	luaL_requiref(L, "huxdemp", luau_openlib, false);

	// Parse arguments with `arg.h'.
	char *optarg;

	ARGBEGIN {
	break; case 'f':
		/* clear old dfuncs */
		memset(options.dfuncs, 0x0, options.dfuncs_sz * sizeof(options.dfuncs[0]));
		options.dfuncs_sz = 0;

		optarg = EARGF(_usage(argv0));
		char *orig_optarg = strdup(optarg);
		char *ptr = optarg;

		for (size_t i = 0; ptr;) {
			char *column = strsep(&ptr, ",");
			if (*column == '\0') continue;

			strcpy(options.dfunc_names[i], column);

			if (i > ARRAY_LEN(options.dfuncs)) {
				errx(1, "-f recieved more than %zu items. "
					"what were you trying to do anyway?",
					ARRAY_LEN(options.dfuncs));
			}

			if (!strcmp(column, "offset"))
				options.dfuncs[i] = CO_Offset;
			else if (!strcmp(column, "bytes"))
				options.dfuncs[i] = CO_Bytes;
			else if (!strcmp(column, "bytes-left"))
				options.dfuncs[i] = CO_BytesLeft;
			else if (!strcmp(column, "bytes-right"))
				options.dfuncs[i] = CO_BytesRight;
			else if (!strcmp(column, "ascii"))
				options.dfuncs[i] = CO_Ascii;
			else if (!strcmp(column, "ascii-left"))
				options.dfuncs[i] = CO_AsciiLeft;
			else if (!strcmp(column, "ascii-right"))
				options.dfuncs[i] = CO_AsciiRight;
			else {
				char *dash = strchr(column, '-');
				if (dash) *dash = '\0';

				_Bool found_embedded = false;
				for (size_t i = 0; i < ARRAY_LEN(embedded_files); ++i) {
					if (!strcmp(column, embedded_files[i].name)) {
						luau_evalstring(L,
							embedded_files[i].name,
							embedded_files[i].path,
							embedded_files[i].data
						);
						found_embedded = true;
					}
				}

				if (!found_embedded) {
					lua_pushstring(L, (const char *)column);
					luau_call(L, NULL, "require", 1, 1);
					lua_setglobal(L, column);
				}

				options.dfuncs[i] = CO_Plugin;
			}

			++i;
			options.dfuncs_sz = i;
		}

		free(orig_optarg);
	break; case 'l':
		optarg = EARGF(_usage(argv0));
		options.linelen = strtol(optarg, NULL, 0);
		if (options.linelen > MAX_LINELEN) {
			warnx("%ld are much too many bytes for you, sorry",
				options.linelen);
			options.linelen = MAX_LINELEN;
		}
	break; case 's':
		optarg = EARGF(_usage(argv0));
		options.offset = strtol(optarg, NULL, 0);
	break; case 'n':
		optarg = EARGF(_usage(argv0));
		options.length = strtol(optarg, NULL, 0);
	break; case 'c':
		options.ctrls = !options.ctrls;
	break; case 'u':
		options.utf8  = !options.utf8;
	break; case 't':
		optarg = EARGF(_usage(argv0));
		if (!strncmp(optarg, "cp", 2))
			options.table = (char **)&t_cp437;
		else if (!strncmp(optarg, "de", 2))
			options.table = (char **)&t_default;
		else if (!strncmp(optarg, "cl", 2))
			options.table = NULL;
		else
			_usage(argv0);
	break; case 'P':
		optarg = EARGF(_usage(argv0));
		if (!strncmp(optarg, "au", 2))
			options.pager = AM_Auto;
		else if (!strncmp(optarg, "al", 2))
			options.pager = AM_Always;
		else if (!strncmp(optarg, "ne", 2))
			options.pager = AM_Never;
		else
			_usage(argv0);
	break; case 'C':
		optarg = EARGF(_usage(argv0));
		if (!strncmp(optarg, "au", 2))
			options.color = AM_Auto;
		else if (!strncmp(optarg, "al", 2))
			options.color = AM_Always;
		else if (!strncmp(optarg, "ne", 2))
			options.color = AM_Never;
		else
			_usage(argv0);
	break; case 'v': case 'V':
		printf("huxd v"VERSION"\n");
		return 0;
	break; case 'h': case '?': default:
		_usage(argv0);
	} ARGEND

	// Now check whether we can use colors, depending on the user's input
	// (if any). If so, set default colors and parse environment variables.
	options._color = _decide_color();

	if (options._color) {
		config(default_colors);
		config(getenv("HUXD_COLORS") ?: "");
	}

	// Setup a pager. pager_fp will be passed to pclose() later on.
	FILE *pager_fp = pager(options.pager);

	// Now process 'free' arguments the same way every sane POSIX application does: if
	// it's a lone dash, or there are no arguments, read from stdin; otherwise, treat
	// the argument as a file.
	if (!argc) {
		huxdemp("-", pager_fp);
	} else {
		for (; *argv; --argc, ++argv)
			huxdemp(*argv, pager_fp);
	}

	// Close pager_fp, waiting for the pager to exit.
	if (pager_fp != stdout) {
		int r = pclose(pager_fp);
		if (r != 0) {
			warnx("warn: less exited with an error, possibly because it couldn't be found.");
			warnx("hint: use `-P never` to disable using less(1).");
		}
	}

	return 0;
}
