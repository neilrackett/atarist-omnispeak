/*
Omnispeak: A Commander Keen Reimplementation
Copyright (C) 2012 David Gow <david@ingeniumdigital.com>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "id_str.h"
#include "id_mm.h"
#include "id_us.h"
#include "ck_cross.h"
#include "ck_def.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* String manager, allows objects to be indexed by strings */

// Hash a string using an xor variant of the djb2 hash
static unsigned int STR_HashString(const char *str)
{
	unsigned int hash = 5381;
	for (; *str; ++str)
	{
		// unsigned char, so the byte does not go through a sign
		// extension to long on every character.
		hash = ((hash << 5) + hash) ^ (unsigned int)(unsigned char)(*str);
	}
	// The finaliser mixes the high half down so the masked low bits
	// spread. It was a 32-bit multiply, which is a __mulsi3 library
	// call on the 68000; gcc turns this shift into a SWAP.
	hash ^= hash >> 16;
	return hash;
}

// Allocate a table 'tabl' of size 'size'
void STR_AllocTable(STR_Table **tabl, size_t size)
{
	// Round up to a power of two so every lookup can mask instead of
	// dividing: a modulo is a library call on a 68000 and this is the
	// hot loop of start-up parsing. Enforced once here rather than
	// re-derived on each probe.
	size_t pow2 = 1;
	while (pow2 < size)
		pow2 <<= 1;
	size = pow2;

	MM_GetPtr((mm_ptr_t *)(tabl), sizeof(STR_Table) + size * (sizeof(STR_Entry)));
	// Lock it in memory so that it doesn't get purged.
	MM_SetLock((mm_ptr_t *)(tabl), true);
	(*tabl)->size = size;
	(*tabl)->mask = size - 1;
#ifdef CK_DEBUG
	(*tabl)->numElements = 0;
#endif
	for (size_t i = 0; i < size; ++i)
	{
		(*tabl)->arr[i].str = 0;
		(*tabl)->arr[i].ptr = 0;
	}
}

size_t STR_GetEntryIndex(STR_Table *tabl, const char *str)
{
	// STR_AllocTable guarantees a power-of-two size, so this is a mask.
	size_t mask = tabl->mask;
	int hash = STR_HashString(str) & mask;
	int lastHash = -1;
	for (size_t i = hash; i != lastHash; i = (i + 1) & mask)
	{
		if (tabl->arr[i].str == 0)
		{
			return i;
		}
		else if (!strcmp(tabl->arr[i].str, str))
		{
			return i;
		}
		lastHash = hash;
	}
	return ID_STR_INVALID_INDEX;
}

// Checks if an entry 'str' in 'tabl' exists.
bool STR_DoesEntryExist(STR_Table *tabl, const char *str)
{
	size_t index = STR_GetEntryIndex(tabl, str);
	if (index == -1)
		return false;
	if (tabl->arr[index].str)
		return true;
	return false;
}

// Returns the pointer associated with 'str' in 'tabl', defaulting to 'def'
void *STR_LookupEntryWithDefault(STR_Table *tabl, const char *str, void *def)
{
	size_t i = STR_GetEntryIndex(tabl, str);
	if (i == ID_STR_INVALID_INDEX || tabl->arr[i].str == 0)
		return def;
	return (tabl->arr[i].ptr);
}

// Returns the pointer associated with 'str' in 'tabl'
void *STR_LookupEntry(STR_Table *tabl, const char *str)
{
	return STR_LookupEntryWithDefault(tabl, str, (void *)(0));
}

// Add an entry 'str' with pointer 'value' to 'tabl'. Returns 'true' on success
bool STR_AddEntry(STR_Table *tabl, const char *str, void *value)
{
	size_t i = STR_GetEntryIndex(tabl, str);
	if (i == ID_STR_INVALID_INDEX)
	{
		// We've run out of space!
		return false;
	}
	else
	{
#ifdef CK_DEBUG
		if (tabl->arr[i].str != 0)
		{
			CK_Cross_LogMessage(CK_LOG_MSG_WARNING, "STR_AddEntry(\"%s\", %p): Overwriting an entry (\"%s\", %p)! This is probably a memory leak!\n", str, value, tabl->arr[i].str, tabl->arr[i].ptr);
		}
#endif
		tabl->arr[i].str = str;
		tabl->arr[i].ptr = value;
#ifdef CK_DEBUG
		tabl->numElements++;
		if (tabl->numElements == tabl->size)
		{
			Quit("Tried to over-fill a hashtable!");
		}
#endif
		return true;
	}
	return false;
}

// Iterate through the entires in the hashtable. "index" will be updated afterwards.
void *STR_GetNextEntry(STR_Table *tabl, size_t *index)
{
	for (size_t i = *index;; i++)
	{
		if (i >= tabl->size)
		{
			// We're done.
			*index = 0;
			return (void *)0;
		}
		if (tabl->arr[i].str != 0)
		{
			*index = i + 1;
			return (tabl->arr[i].ptr);
		}
	}
}

// The characters the parser treats as whitespace. A plain test rather
// than isspace(): the tokenizer runs over every byte of the data files,
// and a locale-aware ctype call per byte was most of a load on a 68000.
static inline bool STR_IsSpace(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// The whitespace skip and the token scan walk the buffer through a
// local pointer rather than through STR_PeekCharacter/STR_GetCharacter:
// those are calls per byte, and the data files are read once at start-up
// on machines where every call counts.
static void STR_SkipWhitespace(STR_ParserState *ps)
{
	const char *data = ps->data;
	size_t i = ps->dataindex, end = ps->datasize;
	int lines = 0;
	while (i < end)
	{
		char c = data[i];
		if (c == '#')
		{
			// Comments starting with '#' and ending with '\n'
			while (i < end && data[i] != '\n')
				i++;
		}
		else if (STR_IsSpace(c))
		{
			if (c == '\n')
				lines++;
			i++;
		}
		else
			break;
	}
	ps->dataindex = i;
	ps->linecount += lines;
}

STR_Token STR_GetToken(STR_ParserState *ps)
{
	// Return a buffered token if we have one.
	if (ps->haveBufferedToken)
	{
		ps->haveBufferedToken = false;
		return ps->bufferedToken;
	}
	STR_SkipWhitespace(ps);
	STR_Token tok;
	const char *data = ps->data;
	size_t i = ps->dataindex, end = ps->datasize;
	tok.tokenType = STR_TOK_EOF;
	tok.firstIndex = i;
	if (i < end && data[i] == '"')
	{
		// This is a string: it ends at the next unescaped quote. The
		// value is unescaped later, by STR_GetStringValue.
		tok.tokenType = STR_TOK_String;
		i++;
		while (i < end && data[i] != '"')
		{
			if (data[i] == '\\')
				i++;
			if (data[i] == '\n')
				ps->linecount++;
			i++;
		}
		if (i - tok.firstIndex >= ID_STR_MAX_TOKEN_LENGTH)
			Quit("Token exceeded max length!");
		if (i < end)
			i++;
	}
	else if (i < end)
	{
		tok.tokenType = STR_TOK_Ident;
		do
		{
			i++;
		} while (i < end && !STR_IsSpace(data[i]) && data[i] != ',');
		if (i - tok.firstIndex >= ID_STR_MAX_TOKEN_LENGTH)
			Quit("Token exceeded max length!");
	}
	ps->dataindex = i;
	tok.lastIndex = i;

	tok.valuePtr = &ps->data[tok.firstIndex];
	tok.valueLength = tok.lastIndex - tok.firstIndex;
	return tok;
}

STR_Token STR_PeekToken(STR_ParserState *ps)
{
	STR_Token tok = STR_GetToken(ps);

	// Unget the token.
	ps->haveBufferedToken = true;
	ps->bufferedToken = tok;

	return tok;
}

/* Parses the string value out of a token, storing the result in memory from destArena */
size_t STR_GetStringValue(STR_Token tok, char *tokenBuf, size_t bufLength)
{
	int i = 0;
	
	if (tok.tokenType == STR_TOK_EOF)
		return 0;
	
	if (tok.tokenType == STR_TOK_String)
	{
		tok.valuePtr++;
		while (*(tok.valuePtr) != '"')
		{
			char c = *(tok.valuePtr++);
			if (c == '\\')
			{
				c = *(tok.valuePtr++);
				switch (c)
				{
				case 'n':
					c = '\n';
					break;
				default:
					// c is now whatever was escaped (e.g. '\')
					break;
				}
			}
			tokenBuf[i++] = c;
			if (i == bufLength)
				Quit("Token exceeded max length!");
		}
	}
	else
	{
		if (tok.valueLength >= bufLength)
			Quit("Token exceeded max length!");
		memcpy(tokenBuf, tok.valuePtr, tok.valueLength);
		i = tok.valueLength;
	}
	tokenBuf[i] = '\0';

	return i;
}

size_t STR_GetString(STR_ParserState *ps, char *tokenBuf, size_t bufLength)
{
	STR_Token tok = STR_GetToken(ps);
	return STR_GetStringValue(tok, tokenBuf, bufLength);
}

size_t STR_GetIdent(STR_ParserState *ps, char *tokenBuf, size_t bufLength)
{
	STR_Token tok = STR_GetToken(ps);
	return STR_GetStringValue(tok, tokenBuf, bufLength);
}

bool STR_IsTokenIdent(STR_Token tok, const char *str)
{
	size_t len = strlen(str);
	if (len != tok.valueLength)
		return false;
	
	return !strncmp(tok.valuePtr, str, len);
}

bool STR_IsTokenIdentCase(STR_Token tok, const char *str)
{
	size_t len = strlen(str);
	if (len != tok.valueLength)
		return false;
	
	return !CK_Cross_strncasecmp(tok.valuePtr, str, len);
}

int STR_GetIntegerValue(STR_Token token)
{
	int result = 0;

	// NOTE: For the time being,
	if (token.tokenType != STR_TOK_Ident && token.tokenType != STR_TOK_Number)
		return 0;

	/* A hand-rolled strtol: decimal, 0x-prefixed hex and '$'-prefixed
	 * hex, with an optional sign. strtol itself was a measurable share
	 * of the start-up parse on a 68000. */
	const char *p = token.valuePtr;
	const char *end = p + token.valueLength;
	bool negative = false;
	int base = 10;
	if (p < end && (*p == '-' || *p == '+'))
	{
		negative = (*p == '-');
		p++;
	}
	if (p < end && *p == '$')
	{
		base = 16;
		p++;
	}
	else if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
	{
		base = 16;
		p += 2;
	}
	for (; p < end; ++p)
	{
		char c = *p;
		int digit;
		if (c >= '0' && c <= '9')
			digit = c - '0';
		else if (base == 16 && c >= 'a' && c <= 'f')
			digit = c - 'a' + 10;
		else if (base == 16 && c >= 'A' && c <= 'F')
			digit = c - 'A' + 10;
		else
			break;
		result = (base == 10) ? (result << 3) + (result << 1) + digit : (result << 4) + digit;
	}
	return negative ? -result : result;
}

int STR_GetInteger(STR_ParserState *ps)
{
	STR_Token token = STR_GetToken(ps);
	return STR_GetIntegerValue(token);
}

bool STR_ExpectToken(STR_ParserState *ps, const char *str)
{
	STR_Token tok = STR_GetToken(ps);

	// An EOF is never what we expect.
	if (tok.tokenType == STR_TOK_EOF)
		return false;

	bool result = !strncmp(tok.valuePtr, str, tok.valueLength);
	//TODO: ValuePtr may not be NULL-terminated in the future.
	if (!result)
		CK_Cross_LogMessage(CK_LOG_MSG_WARNING, "ExpectToken, got \"%s\" expected \"%s\" on line %d\n", tok.valuePtr, str, ps->linecount);
	return result;
}
