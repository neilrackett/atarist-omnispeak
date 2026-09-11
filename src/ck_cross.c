#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include "id_in.h"
#include "id_sd.h"
#include "id_us.h"
#include "ck_cross.h"

#ifdef CK_RAND_DEBUG
#include <execinfo.h>
#endif

const char *ck_cross_logLevel_strings[] = {
	"normal",
	"warning",
	"error",
	"none"
};

CK_Log_Message_Class_T ck_cross_logLevel = CK_DEFAULT_LOG_LEVEL;

#ifdef VL_EGA
#include <unistd.h>
extern bool vl_started;
#endif

#ifdef VL_STDL
#include <time.h>
// On the ST the console is the game screen, so messages go to a file
// next to the program.
static FILE *ck_cross_logFile;
static const char *ck_cross_logPrefix[] = {"", "Warning: ", "Error: ", ""};
#endif

void CK_PRINTF_FORMAT(2, 3) CK_Cross_LogMessage(CK_Log_Message_Class_T msgClass, const char *format, ...)
{
	// TODO: For now we simply do this.
	va_list args;

#ifdef VL_STDL
	// The file is opened on the first call whatever the level, so a
	// run always leaves a log behind and says which level it kept.
	if (!ck_cross_logFile)
	{
		ck_cross_logFile = fopen("OMNISPK.LOG", "w");
		if (ck_cross_logFile)
		{
			fprintf(ck_cross_logFile, "Omnispeak for Atari STE (log level %d)\n", (int)ck_cross_logLevel);
			fflush(ck_cross_logFile);
		}
	}
#endif
	// Only print messages for the enabled log level.
	if (msgClass < ck_cross_logLevel)
		return;
	va_start(args, format);
#ifdef VL_STDL
	if (ck_cross_logFile)
	{
		fputs(ck_cross_logPrefix[msgClass], ck_cross_logFile);
		vfprintf(ck_cross_logFile, format, args);
		fflush(ck_cross_logFile);
	}
#ifdef CK_DEBUG
	// Debug builds also trace to the console, which Hatari can capture.
	va_end(args);
	va_start(args, format);
	fprintf(stderr, "[%6lu] %s", (unsigned long)clock(), ck_cross_logPrefix[msgClass]);
	vfprintf(stderr, format, args);
#endif
	va_end(args);
	return;
#endif
	switch (msgClass)
	{
	case CK_LOG_MSG_NORMAL:
#ifdef VL_EGA
		if (vl_started && isatty(STDOUT_FILENO)) break;
#endif
		vprintf(format, args);
		break;
	case CK_LOG_MSG_WARNING:
#ifdef VL_EGA
		if (vl_started && isatty(STDERR_FILENO)) break;
#endif
		fprintf(stderr, "Warning: ");
		vfprintf(stderr, format, args);
		break;
	case CK_LOG_MSG_ERROR:
#ifdef VL_EGA
		if (vl_started && isatty(STDERR_FILENO)) break;
#endif
		fprintf(stderr, "Error: ");
		vfprintf(stderr, format, args);
		break;
	}
	va_end(args);
}

#ifdef CK_RAND_DEBUG

void CK_Cross_StackTrace()
{
	int numFunctions;
	char **strings;
	void *buffer[100];

	numFunctions = backtrace(buffer, 100);
	strings = backtrace_symbols(buffer, numFunctions);
	for (int i = 0; i < numFunctions; ++i)
		CK_Cross_LogMessage(CK_LOG_MSG_NORMAL, "[BT] %s\n", strings[i]);
}

#endif

void CK_Cross_puts(const char *str)
{
	// Reason for this wrapper: Maybe a different
	// mechanism will be used in the future.
	puts(str);
}

int CK_Cross_toupper(int c)
{
	return ((c >= 'a') && (c <= 'z')) ? (c - 'a' + 'A') : c;
}

int CK_Cross_strcasecmp(const char *s1, const char *s2)
{
	unsigned char uc1, uc2;
	/* This one is easy. We don't care if a value is signed or not. */
	/* All that matters here is consistency (everything is signed). */
	for (; (*s1) && (CK_Cross_toupper(*s1) == CK_Cross_toupper(*s2)); s1++, s2++)
		;
	/* But now, first we cast from int to char, and only *then* to */
	/* unsigned char, so the correct difference can be calculated. */
	uc1 = (unsigned char)((char)(CK_Cross_toupper(*s1)));
	uc2 = (unsigned char)((char)(CK_Cross_toupper(*s2)));
	/* We should still cast back to int, for a signed difference. */
	/* Assumption: An int can store any unsigned char value.      */
	return ((int)uc1 - (int)uc2);
}

int CK_Cross_strncasecmp(const char *s1, const char *s2, size_t n)
{
	unsigned char uc1, uc2;
	/* This one is easy. We don't care if a value is signed or not. */
	/* All that matters here is consistency (everything is signed). */
	n--;
	for (; n && (*s1) && (CK_Cross_toupper(*s1) == CK_Cross_toupper(*s2)); s1++, s2++, n--)
		;
	/* But now, first we cast from int to char, and only *then* to */
	/* unsigned char, so the correct difference can be calculated. */
	uc1 = (unsigned char)((char)(CK_Cross_toupper(*s1)));
	uc2 = (unsigned char)((char)(CK_Cross_toupper(*s2)));
	/* We should still cast back to int, for a signed difference. */
	/* Assumption: An int can store any unsigned char value.      */
	return ((int)uc1 - (int)uc2);
}

size_t CK_Cross_strscpy(char* dst, const char* src, size_t bufsiz)
{
	for (int len = 0; len < bufsiz; ++len)
	{
		dst[len] = src[len];
		if (!src[len])
			return len;
	}
	Quit("strscpy: Buffer too small!");
};
