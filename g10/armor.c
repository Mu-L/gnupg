/* armor.c - Armor filter
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006,
 *               2007 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "gpg.h"
#include "../common/status.h"
#include "../common/iobuf.h"
#include "../common/util.h"
#include "filter.h"
#include "packet.h"
#include "options.h"
#include "main.h"
#include "../common/i18n.h"

#define MAX_LINELEN 20000

static const byte bintoasc[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                               "abcdefghijklmnopqrstuvwxyz"
                               "0123456789+/";
static u32 asctobin[4][256]; /* runtime initialized */
static int is_initialized;


typedef enum {
    fhdrHASArmor = 0,
    fhdrNOArmor,
    fhdrINIT,
    fhdrINITCont,
    fhdrINITSkip,
    fhdrCHECKBegin,
    fhdrWAITHeader,
    fhdrWAITClearsig,
    fhdrSKIPHeader,
    fhdrCLEARSIG,
    fhdrREADClearsig,
    fhdrNullClearsig,
    fhdrEMPTYClearsig,
    fhdrCHECKClearsig,
    fhdrCHECKClearsig2,
    fhdrCHECKDashEscaped,
    fhdrCHECKDashEscaped2,
    fhdrCHECKDashEscaped3,
    fhdrREADClearsigNext,
    fhdrENDClearsig,
    fhdrENDClearsigHelp,
    fhdrTESTSpaces,
    fhdrCLEARSIGSimple,
    fhdrCLEARSIGSimpleNext,
    fhdrTEXT,
    fhdrTEXTSimple,
    fhdrERROR,
    fhdrERRORShow,
    fhdrEOF
} fhdr_state_t;


/* if we encounter this armor string with this index, go
 * into a mode which fakes packets and wait for the next armor */
#define BEGIN_SIGNATURE 2
#define BEGIN_SIGNED_MSG_IDX 3
static char *head_strings[] = {
    "BEGIN PGP MESSAGE",
    "BEGIN PGP PUBLIC KEY BLOCK",
    "BEGIN PGP SIGNATURE",
    "BEGIN PGP SIGNED MESSAGE",
    "BEGIN PGP ARMORED FILE",       /* gnupg extension */
    "BEGIN PGP PRIVATE KEY BLOCK",
    "BEGIN PGP SECRET KEY BLOCK",   /* only used by pgp2 */
    NULL
};
static char *tail_strings[] = {
    "END PGP MESSAGE",
    "END PGP PUBLIC KEY BLOCK",
    "END PGP SIGNATURE",
    "END dummy",
    "END PGP ARMORED FILE",
    "END PGP PRIVATE KEY BLOCK",
    "END PGP SECRET KEY BLOCK",
    NULL
};


static int armor_filter ( void *opaque, int control,
                          iobuf_t chain, byte *buf, size_t *ret_len);




/* Create a new context for armor filters.  */
armor_filter_context_t *
new_armor_context (void)
{
  armor_filter_context_t *afx;
  gpg_error_t err;

  afx = xcalloc (1, sizeof *afx);
  if (afx)
    {
      err = gcry_md_open (&afx->crc_md, GCRY_MD_CRC24_RFC2440, 0);
      if (err != 0)
	{
	  log_error ("gcry_md_open failed for GCRY_MD_CRC24_RFC2440: %s",
		    gpg_strerror (err));
	  xfree (afx);
	  return NULL;
	}

      afx->refcount = 1;
    }

  return afx;
}

/* Release an armor filter context.  Passing NULL is explicitly
   allowed and a no-op.  */
void
release_armor_context (armor_filter_context_t *afx)
{
  if (!afx)
    return;
  log_assert (afx->refcount);
  if ( --afx->refcount )
    return;
  gcry_md_close (afx->crc_md);
  xfree (afx);
}

/* Push the armor filter onto the iobuf stream IOBUF.  */
int
push_armor_filter (armor_filter_context_t *afx, iobuf_t iobuf)
{
  int rc;

  afx->refcount++;
  rc = iobuf_push_filter (iobuf, armor_filter, afx);
  if (rc)
    afx->refcount--;
  return rc;
}


/* This function returns true if the armor filter detected that the
 * input was indeed armored.  Gives a valid result only after the
 * first PGP packet has been read.  */
int
was_armored (armor_filter_context_t *afx)
{
  return (afx && !afx->inp_bypass);
}



static void
initialize(void)
{
    u32 i;
    const byte *s;

    /* Build the helptable for radix64 to bin conversion.  Value 0xffffffff is
       used to detect invalid characters.  */
    memset (asctobin, 0xff, sizeof(asctobin));
    for(s=bintoasc,i=0; *s; s++,i++ )
      {
	asctobin[0][*s] = i << (0 * 6);
	asctobin[1][*s] = i << (1 * 6);
	asctobin[2][*s] = i << (2 * 6);
	asctobin[3][*s] = i << (3 * 6);
      }

    is_initialized=1;
}


static inline u32
get_afx_crc (armor_filter_context_t *afx)
{
  const byte *crc_buf;
  u32 crc;

  crc_buf = gcry_md_read (afx->crc_md, GCRY_MD_CRC24_RFC2440);

  crc = crc_buf[0];
  crc <<= 8;
  crc |= crc_buf[1];
  crc <<= 8;
  crc |= crc_buf[2];

  return crc;
}


/*
 * Check whether this is an armored file.  See also
 * parse-packet.c for details on this code.
 *
 * Note that the buffer BUF needs to be at least 2 bytes long.  If in
 * doubt that the second byte to 0.
 *
 * Returns: True if it seems to be armored
 */
static int
is_armored (const byte *buf)
{
  int ctb, pkttype;
  int indeterminate_length_allowed;

    ctb = *buf;
    if( !(ctb & 0x80) )
      /* The most significant bit of the CTB must be set.  Since it is
         cleared, this is not a binary OpenPGP message.  Assume it is
         armored.  */
      return 1;

    pkttype =  ctb & 0x40 ? (ctb & 0x3f) : ((ctb>>2)&0xf);
    switch( pkttype ) {
      case PKT_PUBKEY_ENC:
      case PKT_SIGNATURE:
      case PKT_SYMKEY_ENC:
      case PKT_ONEPASS_SIG:
      case PKT_SECRET_KEY:
      case PKT_PUBLIC_KEY:
      case PKT_SECRET_SUBKEY:
      case PKT_MARKER:
      case PKT_RING_TRUST:
      case PKT_USER_ID:
      case PKT_PUBLIC_SUBKEY:
      case PKT_ATTRIBUTE:
      case PKT_MDC:
	indeterminate_length_allowed = 0;
        break;

      case PKT_COMPRESSED:
      case PKT_ENCRYPTED:
      case PKT_ENCRYPTED_MDC:
      case PKT_ENCRYPTED_AEAD:
      case PKT_PLAINTEXT:
      case PKT_OLD_COMMENT:
      case PKT_COMMENT:
      case PKT_GPG_CONTROL:
	indeterminate_length_allowed = 1;
        break;

      default:
        /* Invalid packet type.  */
        return 1;
    }

    if (! indeterminate_length_allowed)
      /* It is only legal to use an indeterminate length with a few
         packet types.  If a packet uses an indeterminate length, but
         that is not allowed, then the data is not valid binary
         OpenPGP data.  */
      {
        int new_format;
        int indeterminate_length;

        new_format = !! (ctb & (1 << 6));
        if (new_format)
          indeterminate_length = (buf[1] >= 224 && buf[1] < 255);
        else
          indeterminate_length = (ctb & 3) == 3;

        if (indeterminate_length)
          return 1;
      }

    /* The first CTB seems legit.  It is probably not armored
       data.  */
    return 0;
}


/****************
 * Try to check whether the iobuf is armored
 * Returns true if this may be the case; the caller should use the
 *	   filter to do further processing.
 */
int
use_armor_filter( IOBUF a )
{
    byte buf[2];
    int n;

    /* fixme: there might be a problem with iobuf_peek */
    n = iobuf_peek (a, buf, 2);
    if( n == -1 )
	return 0; /* EOF, doesn't matter whether armored or not */
    if( !n )
	return 1; /* can't check it: try armored */
    if (n != 2)
	return 0; /* short buffer */
    return is_armored(buf);
}




static void
invalid_armor(void)
{
    write_status(STATUS_BADARMOR);
    g10_exit(1); /* stop here */
}


/****************
 * check whether the armor header is valid on a signed message.
 * this is for security reasons: the header lines are not included in the
 * hash and by using some creative formatting rules, Mallory could fake
 * any text at the beginning of a document; assuming it is read with
 * a simple viewer. We only allow the Hash Header.
 */
static int
parse_hash_header( const char *line )
{
    const char *s, *s2;
    unsigned found = 0;

    if( strlen(line) < 6  || strlen(line) > 60 )
	return 0; /* too short or too long */
    if( memcmp( line, "Hash:", 5 ) )
	return 0; /* invalid header */

    for(s=line+5;;s=s2) {
	for(; *s && (*s==' ' || *s == '\t'); s++ )
	    ;
	if( !*s )
	    break;
	for(s2=s+1; *s2 && *s2!=' ' && *s2 != '\t' && *s2 != ','; s2++ )
	    ;
	if( !strncmp( s, "RIPEMD160", s2-s ) )
	    found |= 1;
	else if( !strncmp( s, "SHA1", s2-s ) )
	    found |= 2;
	else if( !strncmp( s, "SHA224", s2-s ) )
	    found |= 8;
	else if( !strncmp( s, "SHA256", s2-s ) )
	    found |= 16;
	else if( !strncmp( s, "SHA384", s2-s ) )
	    found |= 32;
	else if( !strncmp( s, "SHA512", s2-s ) )
	    found |= 64;
	else
	    return 0;
	for(; *s2 && (*s2==' ' || *s2 == '\t'); s2++ )
	    ;
	if( *s2 && *s2 != ',' )
	    return 0;
	if( *s2 )
	    s2++;
    }
    return found;
}

/* Returns true if this is a valid armor tag as per RFC-2440bis-21. */
static int
is_armor_tag(const char *line)
{
  if(strncmp(line,"Version",7)==0
     || strncmp(line,"Comment",7)==0
     || strncmp(line,"MessageID",9)==0
     || strncmp(line,"Hash",4)==0
     || strncmp(line,"Charset",7)==0)
    return 1;

  return 0;
}

/****************
 * Check whether this is a armor line.  Returns: -1 if it is not a
 * armor header, 42 if it is a generic header, or the index number of
 * the armor header.
 */
static int
is_armor_header( byte *line, unsigned len )
{
    const char *s;
    byte *save_p, *p;
    int save_c;
    int i;

    if( len < 15 )
	return -1; /* too short */
    if( memcmp( line, "-----", 5 ) )
	return -1; /* no */
    p = strstr( line+5, "-----");
    if( !p )
	return -1;
    save_p = p;
    p += 5;

    /* Some Windows environments seem to add whitespace to the end of
       the line, so we strip it here.  This becomes strict if
       --rfc2440 is set since 2440 reads "The header lines, therefore,
       MUST start at the beginning of a line, and MUST NOT have text
       following them on the same line."  It is unclear whether "text"
       refers to all text or just non-whitespace text.  4880 clarified
       this was only non-whitespace text. */

    if(RFC2440)
      {
	if( *p == '\r' )
	  p++;
	if( *p == '\n' )
	  p++;
      }
    else
      while(*p==' ' || *p=='\r' || *p=='\n' || *p=='\t')
	p++;

    if( *p )
	return -1; /* garbage after dashes */
    save_c = *save_p; *save_p = 0;
    p = line+5;
    for(i=0; (s=head_strings[i]); i++ )
	if( !strcmp(s, p) )
	    break;
    *save_p = save_c;
    if (!s)
      {
        if (!strncmp (p, "BEGIN ", 6))
          return 42;
	return -1; /* unknown armor line */
      }

    if( opt.verbose > 1 )
	log_info(_("armor: %s\n"), head_strings[i]);
    return i;
}



/****************
 * Parse a header lines
 * Return 0: Empty line (end of header lines)
 *	 -1: invalid header line
 *	 >0: Good header line
 */
static int
parse_header_line( armor_filter_context_t *afx, byte *line, unsigned int len )
{
    byte *p;
    int hashes=0;
    unsigned int len2;

    len2 = length_sans_trailing_ws ( line, len );
    if( !len2 ) {
        afx->buffer_pos = len2;  /* (it is not the fine way to do it here) */
	return 0; /* WS only: same as empty line */
    }

    /*
      This is fussy.  The spec says that a header line is delimited
      with a colon-space pair.  This means that a line such as
      "Comment: " (with nothing else) is actually legal as an empty
      string comment.  However, email and cut-and-paste being what it
      is, that trailing space may go away.  Therefore, we accept empty
      headers delimited with only a colon.  --rfc2440, as always,
      makes this strict and enforces the colon-space pair. -dms
    */

    p = strchr( line, ':');
    if (!p && afx->dearmor_state)
      return 0; /* Special treatment in --dearmor mode.  */
    if( !p || (RFC2440 && p[1]!=' ')
	|| (!RFC2440 && p[1]!=' ' && p[1]!='\n' && p[1]!='\r'))
      {
	log_error (_("invalid armor header: "));
	es_write_sanitized (log_get_stream (), line, len, NULL, NULL);
	log_printf ("\n");
	return -1;
      }

    /* Chop off the whitespace we detected before */
    len=len2;
    line[len2]='\0';

    if( opt.verbose ) {
	log_info(_("armor header: "));
	es_write_sanitized (log_get_stream (), line, len, NULL, NULL);
	log_printf ("\n");
    }

    if (afx->dearmor_mode)
      ;
    else if (afx->in_cleartext)
      {
	if( (hashes=parse_hash_header( line )) )
	  afx->hashes |= hashes;
	else if( strlen(line) > 15 && !memcmp( line, "NotDashEscaped:", 15 ) )
	  afx->not_dash_escaped = 1;
	else
	  {
	    log_error(_("invalid clearsig header\n"));
	    return -1;
	  }
      }
    else if(!is_armor_tag(line))
      {
	/* Section 6.2: "Unknown keys should be reported to the user,
	   but OpenPGP should continue to process the message."  Note
	   that in a clearsigned message this applies to the signature
	   part (i.e. "BEGIN PGP SIGNATURE") and not the signed data
	   ("BEGIN PGP SIGNED MESSAGE").  The only key allowed in the
	   signed data section is "Hash". */

	log_info(_("unknown armor header: "));
	es_write_sanitized (log_get_stream (), line, len, NULL, NULL);
	log_printf ("\n");
      }

    return 1;
}



/* figure out whether the data is armored or not */
static int
check_input( armor_filter_context_t *afx, IOBUF a )
{
    int rc = 0;
    int i;
    byte *line;
    unsigned len;
    unsigned maxlen;
    int hdr_line = -1;

    /* read the first line to see whether this is armored data */
    maxlen = MAX_LINELEN;
    len = afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					     &afx->buffer_size, &maxlen );
    line = afx->buffer;
    if( !maxlen ) {
	/* line has been truncated: assume not armored */
	afx->inp_checked = 1;
	afx->inp_bypass = 1;
	return 0;
    }

    if( !len ) {
	return -1; /* eof */
    }

    /* (the line is always a C string but maybe longer) */
    if( *line == '\n' || ( len && (*line == '\r' && line[1]=='\n') ) )
	;
    else if (len >= 2 && !is_armored (line)) {
	afx->inp_checked = 1;
	afx->inp_bypass = 1;
	return 0;
    }

    /* find the armor header */
    while(len) {
	i = is_armor_header( line, len );
        if ( i == 42 ) {
            if (afx->dearmor_mode) {
                afx->dearmor_state = 1;
                break;
            }
        }
        else if (i >= 0
                 && !(afx->only_keyblocks && i != 1 && i != 5 && i != 6 )) {
	    hdr_line = i;
	    if( hdr_line == BEGIN_SIGNED_MSG_IDX ) {
	        if( afx->in_cleartext ) {
		    log_error(_("nested clear text signatures\n"));
		    rc = gpg_error (GPG_ERR_INV_ARMOR);
                }
		afx->in_cleartext = 1;
	    }
	    break;
	}

	/* read the next line (skip all truncated lines) */
	do {
	    maxlen = MAX_LINELEN;
	    afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					       &afx->buffer_size, &maxlen );
	    line = afx->buffer;
	    len = afx->buffer_len;
	} while( !maxlen );
    }

    /* Parse the header lines.  */
    while(len) {
	/* Read the next line (skip all truncated lines). */
	do {
	    maxlen = MAX_LINELEN;
	    afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					       &afx->buffer_size, &maxlen );
	    line = afx->buffer;
	    len = afx->buffer_len;
	} while( !maxlen );

	i = parse_header_line( afx, line, len );
	if( i <= 0 ) {
	    if (i && RFC2440)
		rc = GPG_ERR_INV_ARMOR;
	    break;
	}
    }


    if( rc )
	invalid_armor();
    else if( afx->in_cleartext )
	afx->faked = 1;
    else {
	afx->inp_checked = 1;
	gcry_md_reset (afx->crc_md);
	afx->idx = 0;
	afx->radbuf[0] = 0;
    }

    return rc;
}

#define PARTIAL_CHUNK 512
#define PARTIAL_POW   9

/****************
 * Fake a literal data packet and wait for the next armor line
 * fixme: empty line handling and null length clear text signature are
 *	  not implemented/checked.
 */
static int
fake_packet( armor_filter_context_t *afx, IOBUF a,
	     size_t *retn, byte *buf, size_t size  )
{
    int rc = 0;
    size_t len = 0;
    int lastline = 0;
    unsigned maxlen, n;
    byte *p;
    byte tempbuf[PARTIAL_CHUNK];
    size_t tempbuf_len=0;
    int this_truncated;

    while( !rc && size-len>=(PARTIAL_CHUNK+1)) {
	/* copy what we have in the line buffer */
	if( afx->faked == 1 )
	    afx->faked++; /* skip the first (empty) line */
	else
	  {
	    /* It's full, so write this partial chunk */
	    if(tempbuf_len==PARTIAL_CHUNK)
	      {
		buf[len++]=0xE0+PARTIAL_POW;
		memcpy(&buf[len],tempbuf,PARTIAL_CHUNK);
		len+=PARTIAL_CHUNK;
		tempbuf_len=0;
		continue;
	      }

	    while( tempbuf_len < PARTIAL_CHUNK
		   && afx->buffer_pos < afx->buffer_len )
	      tempbuf[tempbuf_len++] = afx->buffer[afx->buffer_pos++];
	    if( tempbuf_len==PARTIAL_CHUNK )
	      continue;
	  }

	/* read the next line */
	maxlen = MAX_LINELEN;
	afx->buffer_pos = 0;
	afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					   &afx->buffer_size, &maxlen );
	if( !afx->buffer_len ) {
	    rc = -1; /* eof (should not happen) */
	    continue;
	}
	if( !maxlen )
          {
	    afx->truncated++;
            this_truncated = 1;
          }
        else
          this_truncated = 0;


	p = afx->buffer;
	n = afx->buffer_len;

	/* Armor header or dash-escaped line? */
	if(p[0]=='-')
	  {
	    /* 2440bis-10: When reversing dash-escaping, an
	       implementation MUST strip the string "- " if it occurs
	       at the beginning of a line, and SHOULD warn on "-" and
	       any character other than a space at the beginning of a
	       line.  */

	    if(p[1]==' ' && !afx->not_dash_escaped)
	      {
		/* It's a dash-escaped line, so skip over the
		   escape. */
		afx->buffer_pos = 2;
	      }
	    else if(p[1]=='-' && p[2]=='-' && p[3]=='-' && p[4]=='-')
	      {
		/* Five dashes in a row mean it's probably armor
		   header. */
		int type = is_armor_header( p, n );
                if (type == 42)
                  type = -1;  /* Only OpenPGP armors are expected.  */
		if( afx->not_dash_escaped && type != BEGIN_SIGNATURE )
		  ; /* this is okay */
		else
		  {
		    if( type != BEGIN_SIGNATURE )
		      {
			log_info(_("unexpected armor: "));
			es_write_sanitized (log_get_stream (), p, n,
                                            NULL, NULL);
			log_printf ("\n");
		      }

		    lastline = 1;
		    rc = -1;
		  }
	      }
	    else if(!afx->not_dash_escaped)
	      {
		/* Bad dash-escaping. */
		log_info (_("invalid dash escaped line: "));
		es_write_sanitized (log_get_stream (), p, n, NULL, NULL);
		log_printf ("\n");
	      }
	  }

	/* Now handle the end-of-line canonicalization */
	if( !afx->not_dash_escaped || this_truncated)
	  {
	    int crlf = n > 1 && p[n-2] == '\r' && p[n-1]=='\n';

	    afx->buffer_len=
	      trim_trailing_chars( &p[afx->buffer_pos], n-afx->buffer_pos,
				   " \t\r\n");
	    afx->buffer_len+=afx->buffer_pos;
	    /* the buffer is always allocated with enough space to append
	     * the removed [CR], LF and a Nul
	     * The reason for this complicated procedure is to keep at least
	     * the original type of lineending - handling of the removed
	     * trailing spaces seems to be impossible in our method
	     * of faking a packet; either we have to use a temporary file
	     * or calculate the hash here in this module and somehow find
	     * a way to send the hash down the processing line (well, a special
	     * faked packet could do the job).
             *
             * To make sure that a truncated line triggers a bad
             * signature error we replace a removed LF by a FF or
             * append a FF.  Right, this is a hack but better than a
             * global variable and way easier than to introduce a new
             * control packet or insert a line like "[truncated]\n"
             * into the filter output.
	     */
	    if( crlf )
	      afx->buffer[afx->buffer_len++] = '\r';
	    afx->buffer[afx->buffer_len++] = this_truncated? '\f':'\n';
	    afx->buffer[afx->buffer_len] = '\0';
	  }
    }

    if( lastline ) { /* write last (ending) length header */
        if(tempbuf_len<192)
	  buf[len++]=tempbuf_len;
	else
	  {
	    buf[len++]=((tempbuf_len-192)/256) + 192;
	    buf[len++]=(tempbuf_len-192) % 256;
	  }
	memcpy(&buf[len],tempbuf,tempbuf_len);
	len+=tempbuf_len;

	rc = 0;
	afx->faked = 0;
	afx->in_cleartext = 0;
	/* and now read the header lines */
	afx->buffer_pos = 0;
	for(;;) {
	    int i;

	    /* read the next line (skip all truncated lines) */
	    do {
		maxlen = MAX_LINELEN;
		afx->buffer_len = iobuf_read_line( a, &afx->buffer,
						 &afx->buffer_size, &maxlen );
	    } while( !maxlen );
	    p = afx->buffer;
	    n = afx->buffer_len;
	    if( !n ) {
		rc = -1;
		break; /* eof */
	    }
	    i = parse_header_line( afx, p , n );
	    if( i <= 0 ) {
		if( i )
		    invalid_armor();
		break;
	    }
	}
	afx->inp_checked = 1;
	gcry_md_reset (afx->crc_md);
	afx->idx = 0;
	afx->radbuf[0] = 0;
    }

    *retn = len;
    return rc;
}


static int
invalid_crc(void)
{
  if ( opt.ignore_crc_error )
    return 0;
  log_inc_errorcount();
  return gpg_error (GPG_ERR_INV_ARMOR);
}


static int
radix64_read( armor_filter_context_t *afx, IOBUF a, size_t *retn,
	      byte *buf, size_t size )
{
    byte val;
    int c;
    u32 binc;
    int checkcrc=0;
    int rc = 0;
    size_t n = 0;
    int idx, onlypad=0;
    int skip_fast = 0;

    idx = afx->idx;
    val = afx->radbuf[0];
    for( n=0; n < size; ) {

	if( afx->buffer_pos < afx->buffer_len )
	    c = afx->buffer[afx->buffer_pos++];
	else { /* read the next line */
	    unsigned maxlen = MAX_LINELEN;
	    afx->buffer_pos = 0;
	    afx->buffer_len = iobuf_read_line( a, &afx->buffer,
					       &afx->buffer_size, &maxlen );
	    if( !maxlen )
		afx->truncated++;
	    if( !afx->buffer_len )
		break; /* eof */
	    continue;
	}

      again:
	binc = asctobin[0][c];

	if( binc != 0xffffffffUL )
	  {
	    if( idx == 0 && skip_fast == 0
		&& afx->buffer_pos + (16 - 1) < afx->buffer_len
		&& n + 12 < size)
	      {
		/* Fast path for radix64 to binary conversion.  */
		u32 b0,b1,b2,b3;

		/* Speculatively load 15 more input bytes.  */
		b0 = binc << (3 * 6);
		b0 |= asctobin[2][afx->buffer[afx->buffer_pos + 0]];
		b0 |= asctobin[1][afx->buffer[afx->buffer_pos + 1]];
		b0 |= asctobin[0][afx->buffer[afx->buffer_pos + 2]];
		b1  = asctobin[3][afx->buffer[afx->buffer_pos + 3]];
		b1 |= asctobin[2][afx->buffer[afx->buffer_pos + 4]];
		b1 |= asctobin[1][afx->buffer[afx->buffer_pos + 5]];
		b1 |= asctobin[0][afx->buffer[afx->buffer_pos + 6]];
		b2  = asctobin[3][afx->buffer[afx->buffer_pos + 7]];
		b2 |= asctobin[2][afx->buffer[afx->buffer_pos + 8]];
		b2 |= asctobin[1][afx->buffer[afx->buffer_pos + 9]];
		b2 |= asctobin[0][afx->buffer[afx->buffer_pos + 10]];
		b3  = asctobin[3][afx->buffer[afx->buffer_pos + 11]];
		b3 |= asctobin[2][afx->buffer[afx->buffer_pos + 12]];
		b3 |= asctobin[1][afx->buffer[afx->buffer_pos + 13]];
		b3 |= asctobin[0][afx->buffer[afx->buffer_pos + 14]];

		/* Check if any of the input bytes were invalid. */
		if( (b0 | b1 | b2 | b3) != 0xffffffffUL )
		  {
		    /* All 16 bytes are valid. */
		    buf[n + 0] = b0 >> (2 * 8);
		    buf[n + 1] = b0 >> (1 * 8);
		    buf[n + 2] = b0 >> (0 * 8);
		    buf[n + 3] = b1 >> (2 * 8);
		    buf[n + 4] = b1 >> (1 * 8);
		    buf[n + 5] = b1 >> (0 * 8);
		    buf[n + 6] = b2 >> (2 * 8);
		    buf[n + 7] = b2 >> (1 * 8);
		    buf[n + 8] = b2 >> (0 * 8);
		    buf[n + 9] = b3 >> (2 * 8);
		    buf[n + 10] = b3 >> (1 * 8);
		    buf[n + 11] = b3 >> (0 * 8);
		    afx->buffer_pos += 16 - 1;
		    n += 12;
		    continue;
		  }
		else if( b0 == 0xffffffffUL )
		  {
		    /* byte[1..3] have invalid character(s).  Switch to slow
		       path.  */
		    skip_fast = 1;
		  }
		else if( b1 == 0xffffffffUL )
		  {
		    /* byte[4..7] have invalid character(s), first 4 bytes are
		       valid.  */
		    buf[n + 0] = b0 >> (2 * 8);
		    buf[n + 1] = b0 >> (1 * 8);
		    buf[n + 2] = b0 >> (0 * 8);
		    afx->buffer_pos += 4 - 1;
		    n += 3;
		    skip_fast = 1;
		    continue;
		  }
		else if( b2 == 0xffffffffUL )
		  {
		    /* byte[8..11] have invalid character(s), first 8 bytes are
		       valid.  */
		    buf[n + 0] = b0 >> (2 * 8);
		    buf[n + 1] = b0 >> (1 * 8);
		    buf[n + 2] = b0 >> (0 * 8);
		    buf[n + 3] = b1 >> (2 * 8);
		    buf[n + 4] = b1 >> (1 * 8);
		    buf[n + 5] = b1 >> (0 * 8);
		    afx->buffer_pos += 8 - 1;
		    n += 6;
		    skip_fast = 1;
		    continue;
		  }
		else /*if( b3 == 0xffffffffUL )*/
		  {
		    /* byte[12..15] have invalid character(s), first 12 bytes
		       are valid.  */
		    buf[n + 0] = b0 >> (2 * 8);
		    buf[n + 1] = b0 >> (1 * 8);
		    buf[n + 2] = b0 >> (0 * 8);
		    buf[n + 3] = b1 >> (2 * 8);
		    buf[n + 4] = b1 >> (1 * 8);
		    buf[n + 5] = b1 >> (0 * 8);
		    buf[n + 6] = b2 >> (2 * 8);
		    buf[n + 7] = b2 >> (1 * 8);
		    buf[n + 8] = b2 >> (0 * 8);
		    afx->buffer_pos += 12 - 1;
		    n += 9;
		    skip_fast = 1;
		    continue;
		  }
	      }

	    switch(idx)
	      {
		case 0: val =  binc << 2; break;
		case 1: val |= (binc>>4)&3; buf[n++]=val;val=(binc<<4)&0xf0;break;
		case 2: val |= (binc>>2)&15; buf[n++]=val;val=(binc<<6)&0xc0;break;
		case 3: val |= binc&0x3f; buf[n++] = val; break;
	      }
	    idx = (idx+1) % 4;

	    continue;
	  }

	skip_fast = 0;

	if( c == '\n' || c == ' ' || c == '\r' || c == '\t' )
	    continue;
	else if( c == '=' ) { /* pad character: stop */
	    /* some mailers leave quoted-printable encoded characters
	     * so we try to workaround this */
	    if( afx->buffer_pos+2 < afx->buffer_len ) {
		int cc1, cc2, cc3;
		cc1 = afx->buffer[afx->buffer_pos];
		cc2 = afx->buffer[afx->buffer_pos+1];
		cc3 = afx->buffer[afx->buffer_pos+2];
		if( isxdigit(cc1) && isxdigit(cc2)
				  && strchr( "=\n\r\t ", cc3 )) {
		    /* well it seems to be the case - adjust */
		    c = isdigit(cc1)? (cc1 - '0'): (ascii_toupper(cc1)-'A'+10);
		    c <<= 4;
		    c |= isdigit(cc2)? (cc2 - '0'): (ascii_toupper(cc2)-'A'+10);
		    afx->buffer_pos += 2;
		    afx->qp_detected = 1;
		    goto again;
		}
	    }

            /* Occasionally a bug MTA will leave the = escaped as
               =3D.  If the 4 characters following that are valid
               Radix64 characters and they are following by a new
               line, assume that this is the case and skip the
               3D.  */
            if (afx->buffer_pos + 6 < afx->buffer_len
                && afx->buffer[afx->buffer_pos + 0] == '3'
                && afx->buffer[afx->buffer_pos + 1] == 'D'
                && asctobin[0][afx->buffer[afx->buffer_pos + 2]] != 0xffffffffUL
                && asctobin[0][afx->buffer[afx->buffer_pos + 3]] != 0xffffffffUL
                && asctobin[0][afx->buffer[afx->buffer_pos + 4]] != 0xffffffffUL
                && asctobin[0][afx->buffer[afx->buffer_pos + 5]] != 0xffffffffUL
                && afx->buffer[afx->buffer_pos + 6] == '\n')
              {
                afx->buffer_pos += 2;
                afx->qp_detected = 1;
              }

	    if (!n)
	      onlypad = 1;

	    if( idx == 1 )
		buf[n++] = val;
	    checkcrc++;
	    break;
	}
        else if (c == '-'
                 && afx->buffer_pos + 8 < afx->buffer_len
                 && !strncmp (afx->buffer, "-----END ", 8)) {
            break; /* End in --dearmor mode or No CRC.  */
        }
	else {
	    log_error(_("invalid radix64 character %02X skipped\n"), c);
	    continue;
	}
    }

    afx->idx = idx;
    afx->radbuf[0] = val;

    if( n )
      {
        gcry_md_write (afx->crc_md, buf, n);
        afx->any_data = 1;
      }

    if( checkcrc ) {
	gcry_md_final (afx->crc_md);
	afx->inp_checked=0;
	afx->faked = 0;
	for(;;) { /* skip lf and pad characters */
	    if( afx->buffer_pos < afx->buffer_len )
		c = afx->buffer[afx->buffer_pos++];
	    else { /* read the next line */
		unsigned maxlen = MAX_LINELEN;
		afx->buffer_pos = 0;
		afx->buffer_len = iobuf_read_line( a, &afx->buffer,
						   &afx->buffer_size, &maxlen );
		if( !maxlen )
		    afx->truncated++;
		if( !afx->buffer_len )
		    break; /* eof */
		continue;
	    }
	    if( c == '\n' || c == ' ' || c == '\r'
		|| c == '\t' || c == '=' )
		continue;
	    break;
	}
	if( !afx->buffer_len )
	    log_error(_("premature eof (no CRC)\n"));
	else {
	    u32 mycrc = 0;
	    idx = 0;
	    do {
		if( (binc = asctobin[0][c]) == 0xffffffffUL )
		    break;
		switch(idx) {
		  case 0: val =  binc << 2; break;
		  case 1: val |= (binc>>4)&3; mycrc |= val << 16;val=(binc<<4)&0xf0;break;
		  case 2: val |= (binc>>2)&15; mycrc |= val << 8;val=(binc<<6)&0xc0;break;
		  case 3: val |= binc&0x3f; mycrc |= val; break;
		}
		for(;;) {
		    if( afx->buffer_pos < afx->buffer_len )
			c = afx->buffer[afx->buffer_pos++];
		    else { /* read the next line */
			unsigned maxlen = MAX_LINELEN;
			afx->buffer_pos = 0;
			afx->buffer_len = iobuf_read_line( a, &afx->buffer,
							   &afx->buffer_size,
								&maxlen );
			if( !maxlen )
			    afx->truncated++;
			if( !afx->buffer_len )
			    break; /* eof */
			continue;
		    }
		    break;
		}
		if( !afx->buffer_len )
		    break; /* eof */
	    } while( ++idx < 4 );
	    if( !afx->buffer_len ) {
		log_info(_("premature eof (in CRC)\n"));
		rc = invalid_crc();
	    }
	    else if( idx == 0 ) {
	        /* No CRC at all is legal ("MAY") */
	        rc=0;
	    }
	    else if( idx != 4 ) {
		log_info(_("malformed CRC\n"));
		rc = invalid_crc();
	    }
	    else if( mycrc != get_afx_crc (afx) ) {
		log_info (_("CRC error; %06lX - %06lX\n"),
				    (ulong)get_afx_crc (afx), (ulong)mycrc);
		rc = invalid_crc();
	    }
	    else {
		rc = 0;
                /* FIXME: Here we should emit another control packet,
                 * so that we know in mainproc that we are processing
                 * a clearsign message */
#if 0
		for(rc=0;!rc;) {
		    rc = 0 /*check_trailer( &fhdr, c )*/;
		    if( !rc ) {
			if( (c=iobuf_get(a)) == -1 )
			    rc = 2;
		    }
		}
		if( rc == -1 )
		    rc = 0;
		else if( rc == 2 ) {
		    log_error(_("premature eof (in trailer)\n"));
		    rc = GPG_ERR_INV_ARMOR;
		}
		else {
		    log_error(_("error in trailer line\n"));
		    rc = GPG_ERR_INV_ARMOR;
		}
#endif
	    }
	}
    }

    if( !n && !onlypad )
	rc = -1;

    *retn = n;
    return rc;
}

static void
armor_output_buf_as_radix64 (armor_filter_context_t *afx, IOBUF a,
			     byte *buf, size_t size)
{
  byte radbuf[sizeof (afx->radbuf)];
  byte outbuf[64 + sizeof (afx->eol)];
  unsigned int eollen = strlen (afx->eol);
  u32 in, in2;
  int idx, idx2;
  int i;

  idx = afx->idx;
  idx2 = afx->idx2;
  memcpy (radbuf, afx->radbuf, sizeof (afx->radbuf));

  if (size && (idx || idx2))
    {
      /* preload eol to outbuf buffer */
      memcpy (outbuf + 4, afx->eol, sizeof (afx->eol));

      for (; size && (idx || idx2); buf++, size--)
	{
	  radbuf[idx++] = *buf;
	  if (idx > 2)
	    {
	      idx = 0;
	      in = (u32)radbuf[0] << (2 * 8);
	      in |= (u32)radbuf[1] << (1 * 8);
	      in |= (u32)radbuf[2] << (0 * 8);
	      outbuf[0] = bintoasc[(in >> 18) & 077];
	      outbuf[1] = bintoasc[(in >> 12) & 077];
	      outbuf[2] = bintoasc[(in >> 6) & 077];
	      outbuf[3] = bintoasc[(in >> 0) & 077];
	      if (++idx2 >= (64/4))
		{ /* pgp doesn't like 72 here */
		  idx2=0;
		  iobuf_write (a, outbuf, 4 + eollen);
		}
	      else
		{
		  iobuf_write (a, outbuf, 4);
		}
	    }
	}
    }

  if (size >= (64/4)*3)
    {
      /* preload eol to outbuf buffer */
      memcpy (outbuf + 64, afx->eol, sizeof(afx->eol));

      do
	{
	  /* idx and idx2 == 0 */

	  for (i = 0; i < (64/8); i++)
	    {
	      in = (u32)buf[0] << (2 * 8);
	      in |= (u32)buf[1] << (1 * 8);
	      in |= (u32)buf[2] << (0 * 8);
	      in2 = (u32)buf[3] << (2 * 8);
	      in2 |= (u32)buf[4] << (1 * 8);
	      in2 |= (u32)buf[5] << (0 * 8);
	      outbuf[i*8+0] = bintoasc[(in >> 18) & 077];
	      outbuf[i*8+1] = bintoasc[(in >> 12) & 077];
	      outbuf[i*8+2] = bintoasc[(in >> 6) & 077];
	      outbuf[i*8+3] = bintoasc[(in >> 0) & 077];
	      outbuf[i*8+4] = bintoasc[(in2 >> 18) & 077];
	      outbuf[i*8+5] = bintoasc[(in2 >> 12) & 077];
	      outbuf[i*8+6] = bintoasc[(in2 >> 6) & 077];
	      outbuf[i*8+7] = bintoasc[(in2 >> 0) & 077];
	      buf+=6;
	      size-=6;
	    }

	  /* pgp doesn't like 72 here */
	  iobuf_write (a, outbuf, 64 + eollen);
	}
      while (size >= (64/4)*3);

      /* restore eol for tail handling */
      if (size)
	memcpy (outbuf + 4, afx->eol, sizeof (afx->eol));
    }

  for (; size; buf++, size--)
    {
      radbuf[idx++] = *buf;
      if (idx > 2)
	{
	  idx = 0;
	  in = (u32)radbuf[0] << (2 * 8);
	  in |= (u32)radbuf[1] << (1 * 8);
	  in |= (u32)radbuf[2] << (0 * 8);
	  outbuf[0] = bintoasc[(in >> 18) & 077];
	  outbuf[1] = bintoasc[(in >> 12) & 077];
	  outbuf[2] = bintoasc[(in >> 6) & 077];
	  outbuf[3] = bintoasc[(in >> 0) & 077];
	  if (++idx2 >= (64/4))
	    { /* pgp doesn't like 72 here */
	      idx2=0;
	      iobuf_write (a, outbuf, 4 + eollen);
	    }
	  else
	    {
	      iobuf_write (a, outbuf, 4);
	    }
	}
    }

  memcpy (afx->radbuf, radbuf, sizeof (afx->radbuf));
  afx->idx = idx;
  afx->idx2 = idx2;
}

/****************
 * This filter is used to handle the armor stuff
 */
static int
armor_filter( void *opaque, int control,
	     IOBUF a, byte *buf, size_t *ret_len)
{
    size_t size = *ret_len;
    armor_filter_context_t *afx = opaque;
    int rc=0, c;
    byte radbuf[3];
    int  idx, idx2;
    size_t n=0;
    u32 crc;
#if 0
    static FILE *fp ;

    if( !fp ) {
	fp = fopen("armor.out", "w");
	log_assert(fp);
    }
#endif

    if( DBG_FILTER )
	log_debug("armor-filter: control: %d\n", control );
    if( control == IOBUFCTRL_UNDERFLOW && afx->inp_bypass ) {
	n = 0;
	if( afx->buffer_len ) {
            /* Copy the data from AFX->BUFFER to BUF.  */
	    for(; n < size && afx->buffer_pos < afx->buffer_len; n++ )
		buf[n++] = afx->buffer[afx->buffer_pos++];
	    if( afx->buffer_pos >= afx->buffer_len )
		afx->buffer_len = 0;
	}
        /* If there is still space in BUF, read directly into it.  */
	for(; n < size; n++ ) {
	    if( (c=iobuf_get(a)) == -1 )
		break;
	    buf[n] = c & 0xff;
	}
	if( !n )
            /* We didn't get any data.  EOF.  */
	    rc = -1;
	*ret_len = n;
    }
    else if( control == IOBUFCTRL_UNDERFLOW ) {
        /* We need some space for the faked packet.  The minimum
         * required size is the PARTIAL_CHUNK size plus a byte for the
         * length itself */
	if( size < PARTIAL_CHUNK+1 )
	    BUG(); /* supplied buffer too short */

	if( afx->faked )
	    rc = fake_packet( afx, a, &n, buf, size );
	else if( !afx->inp_checked ) {
	    rc = check_input( afx, a );
	    if( afx->inp_bypass ) {
		for(n=0; n < size && afx->buffer_pos < afx->buffer_len; )
		    buf[n++] = afx->buffer[afx->buffer_pos++];
		if( afx->buffer_pos >= afx->buffer_len )
		    afx->buffer_len = 0;
		if( !n )
		    rc = -1;
	    }
	    else if( afx->faked ) {
	        unsigned int hashes = afx->hashes;
                const byte *sesmark;
                size_t sesmarklen;

                sesmark = get_session_marker( &sesmarklen );
                if ( sesmarklen > 20 )
                    BUG();

		/* the buffer is at least 15+n*15 bytes long, so it
		 * is easy to construct the packets */

		hashes &= 1|2|8|16|32|64;
		if( !hashes ) {
		    hashes |= 2;  /* Default to SHA-1. */
		}
		n=0;
                /* First a gpg control packet... */
                buf[n++] = 0xff; /* new format, type 63, 1 length byte */
                n++;   /* see below */
                memcpy(buf+n, sesmark, sesmarklen ); n+= sesmarklen;
                buf[n++] = CTRLPKT_CLEARSIGN_START;
                buf[n++] = afx->not_dash_escaped? 0:1; /* sigclass */
                if( hashes & 1 )
                    buf[n++] = DIGEST_ALGO_RMD160;
                if( hashes & 2 )
                    buf[n++] = DIGEST_ALGO_SHA1;
                if( hashes & 8 )
                    buf[n++] = DIGEST_ALGO_SHA224;
                if( hashes & 16 )
                    buf[n++] = DIGEST_ALGO_SHA256;
                if( hashes & 32 )
                    buf[n++] = DIGEST_ALGO_SHA384;
                if( hashes & 64 )
                    buf[n++] = DIGEST_ALGO_SHA512;
                buf[1] = n - 2;

		/* ...followed by an invented plaintext packet.
		   Amusingly enough, this packet is not compliant with
		   2440 as the initial partial length is less than 512
		   bytes.  Of course, we'll accept it anyway ;) */

		buf[n++] = 0xCB; /* new packet format, type 11 */
		buf[n++] = 0xE1; /* 2^1 == 2 bytes */
		buf[n++] = 't';  /* canonical text mode */
		buf[n++] = 0;	 /* namelength */
		buf[n++] = 0xE2; /* 2^2 == 4 more bytes */
		memset(buf+n, 0, 4); /* timestamp */
		n += 4;
	    }
	    else if( !rc )
		rc = radix64_read( afx, a, &n, buf, size );
	}
	else
	    rc = radix64_read( afx, a, &n, buf, size );
#if 0
	if( n )
	    if( fwrite(buf, n, 1, fp ) != 1 )
		BUG();
#endif
	*ret_len = n;
    }
    else if( control == IOBUFCTRL_FLUSH && !afx->cancel ) {
	if( !afx->status ) { /* write the header line */
	    const char *s;
	    strlist_t comment=opt.comments;

	    if( afx->what >= DIM(head_strings) )
		log_bug("afx->what=%d", afx->what);
	    iobuf_writestr(a, "-----");
	    iobuf_writestr(a, head_strings[afx->what] );
	    iobuf_writestr(a, "-----" );
	    iobuf_writestr(a,afx->eol);
	    if (opt.emit_version)
	      {
		iobuf_writestr (a, "Version: "GNUPG_NAME" v");
                for (s=VERSION; *s && *s != '.'; s++)
                  iobuf_writebyte (a, *s);
                if (opt.emit_version > 1 && *s)
                  {
                    iobuf_writebyte (a, *s++);
                    for (; *s && *s != '.'; s++)
                      iobuf_writebyte (a, *s);
                    if (opt.emit_version > 2)
                      {
                        for (; *s && *s != '-' && !spacep (s); s++)
                          iobuf_writebyte (a, *s);
                        if (opt.emit_version > 3)
                          iobuf_writestr (a, " (" PRINTABLE_OS_NAME ")");
                      }
                  }
		iobuf_writestr(a,afx->eol);
	      }

	    /* write the comment strings */
	    for(;comment;comment=comment->next)
	      {
		iobuf_writestr(a, "Comment: " );
		for( s=comment->d; *s; s++ )
		  {
		    if( *s == '\n' )
		      iobuf_writestr(a, "\\n" );
		    else if( *s == '\r' )
		      iobuf_writestr(a, "\\r" );
		    else if( *s == '\v' )
		      iobuf_writestr(a, "\\v" );
		    else
		      iobuf_put(a, *s );
		  }

		iobuf_writestr(a,afx->eol);
	      }

	    if ( afx->hdrlines ) {
                for ( s = afx->hdrlines; *s; s++ ) {
#ifdef HAVE_DOSISH_SYSTEM
                    if ( *s == '\n' )
                        iobuf_put( a, '\r');
#endif
                    iobuf_put(a, *s );
                }
            }

	    iobuf_writestr(a,afx->eol);
	    afx->status++;
	    afx->idx = 0;
	    afx->idx2 = 0;
	    gcry_md_reset (afx->crc_md);
	}

	if( size ) {
	    gcry_md_write (afx->crc_md, buf, size);
	    armor_output_buf_as_radix64 (afx, a, buf, size);
        }
    }
    else if( control == IOBUFCTRL_INIT )
      {
	if( !is_initialized )
	  initialize();

	/* Figure out what we're using for line endings if the caller
	   didn't specify. */
	if(afx->eol[0]==0)
	  {
#ifdef HAVE_DOSISH_SYSTEM
	    afx->eol[0]='\r';
	    afx->eol[1]='\n';
#else
	    afx->eol[0]='\n';
#endif
	  }
      }
    else if( control == IOBUFCTRL_CANCEL ) {
	afx->cancel = 1;
    }
    else if( control == IOBUFCTRL_FREE ) {
	if( afx->cancel )
	    ;
	else if( afx->status ) { /* pad, write checksum, and bottom line */
	    gcry_md_final (afx->crc_md);
	    crc = get_afx_crc (afx);
	    idx = afx->idx;
	    idx2 = afx->idx2;
	    if( idx ) {
		c = bintoasc[(afx->radbuf[0]>>2)&077];
		iobuf_put(a, c);
		if( idx == 1 ) {
		    c = bintoasc[((afx->radbuf[0] << 4) & 060) & 077];
		    iobuf_put(a, c);
		    iobuf_put(a, '=');
		    iobuf_put(a, '=');
		}
		else { /* 2 */
		    c = bintoasc[(((afx->radbuf[0]<<4)&060)
                                  |((afx->radbuf[1]>>4)&017))&077];
		    iobuf_put(a, c);
		    c = bintoasc[((afx->radbuf[1] << 2) & 074) & 077];
		    iobuf_put(a, c);
		    iobuf_put(a, '=');
		}
		if( ++idx2 >= (64/4) )
		  { /* pgp doesn't like 72 here */
		    iobuf_writestr(a,afx->eol);
		    idx2=0;
		  }
	    }
	    /* may need a linefeed */
	    if( idx2 )
	      iobuf_writestr(a,afx->eol);
	    /* write the CRC */
	    iobuf_put(a, '=');
	    radbuf[0] = crc >>16;
	    radbuf[1] = crc >> 8;
	    radbuf[2] = crc;
	    c = bintoasc[(*radbuf >> 2) & 077];
	    iobuf_put(a, c);
	    c = bintoasc[(((*radbuf<<4)&060)|((radbuf[1] >> 4)&017))&077];
	    iobuf_put(a, c);
	    c = bintoasc[(((radbuf[1]<<2)&074)|((radbuf[2]>>6)&03))&077];
	    iobuf_put(a, c);
	    c = bintoasc[radbuf[2]&077];
	    iobuf_put(a, c);
	    iobuf_writestr(a,afx->eol);
	    /* and the trailer */
	    if( afx->what >= DIM(tail_strings) )
		log_bug("afx->what=%d", afx->what);
	    iobuf_writestr(a, "-----");
	    iobuf_writestr(a, tail_strings[afx->what] );
	    iobuf_writestr(a, "-----" );
	    iobuf_writestr(a,afx->eol);
	}
	else if( !afx->any_data && !afx->inp_bypass ) {
	    log_error(_("no valid OpenPGP data found.\n"));
	    afx->no_openpgp_data = 1;
	    write_status_text( STATUS_NODATA, "1" );
	}
        /* Note that in a cleartext signature truncated lines in the
         * plaintext are detected and propagated to the signature
         * checking code by inserting a \f into the plaintext.  We do
         * not use log_info here because some of the truncated lines
         * are harmless.  */
	if( afx->truncated )
	    log_info(_("invalid armor: line longer than %d characters\n"),
		      MAX_LINELEN );
	/* issue an error to enforce dissemination of correct software */
	if( afx->qp_detected )
	    log_error(_("quoted printable character in armor - "
			"probably a buggy MTA has been used\n") );
	xfree( afx->buffer );
	afx->buffer = NULL;
        release_armor_context (afx);
    }
    else if( control == IOBUFCTRL_DESC )
        mem2str (buf, "armor_filter", *ret_len);
    return rc;
}


/****************
 * create a radix64 encoded string.
 */
char *
make_radix64_string( const byte *data, size_t len )
{
    char *buffer, *p;

    buffer = p = xmalloc( (len+2)/3*4 + 1 );
    for( ; len >= 3 ; len -= 3, data += 3 ) {
	*p++ = bintoasc[(data[0] >> 2) & 077];
	*p++ = bintoasc[(((data[0] <<4)&060)|((data[1] >> 4)&017))&077];
	*p++ = bintoasc[(((data[1]<<2)&074)|((data[2]>>6)&03))&077];
	*p++ = bintoasc[data[2]&077];
    }
    if( len == 2 ) {
	*p++ = bintoasc[(data[0] >> 2) & 077];
	*p++ = bintoasc[(((data[0] <<4)&060)|((data[1] >> 4)&017))&077];
	*p++ = bintoasc[((data[1]<<2)&074)];
    }
    else if( len == 1 ) {
	*p++ = bintoasc[(data[0] >> 2) & 077];
	*p++ = bintoasc[(data[0] <<4)&060];
    }
    *p = 0;
    return buffer;
}
