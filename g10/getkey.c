/* getkey.c -  Get a key from the database
 *	Copyright (c) 1997 by Werner Koch (dd9jn)
 *
 * This file is part of G10.
 *
 * G10 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * G10 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "util.h"
#include "packet.h"
#include "memory.h"
#include "iobuf.h"
#include "keydb.h"
#include "options.h"

#define MAX_PKC_CACHE_ENTRIES 500


typedef struct keyid_list {
    struct keyid_list *next;
    u32 keyid[2];
} *keyid_list_t;

typedef struct user_id_db {
    struct user_id_db *next;
    u32 keyid[2];
    int len;
    char name[1];
} *user_id_db_t;

typedef struct pkc_cache_entry {
    struct pkc_cache_entry *next;
    u32 keyid[2];
    PKT_public_cert *pkc;
} *pkc_cache_entry_t;

static STRLIST keyrings;
static STRLIST secret_keyrings;

static keyid_list_t unknown_keyids;
static user_id_db_t user_id_db;
static pkc_cache_entry_t pkc_cache;
static int pkc_cache_entries;	/* number of entries in pkc cache */


static int scan_keyring( PKT_public_cert *pkc, u32 *keyid,
			 const char *name, const char *filename );
static int scan_secret_keyring( PKT_secret_cert *skc, u32 *keyid,
				const char *name, const char *filename);


void
add_keyring( const char *name )
{
    STRLIST sl;
    int rc;

    /* FIXME: check wether this one is available etc */
    /* my be we should do this later */
    sl = m_alloc( sizeof *sl + strlen(name) );
    strcpy(sl->d, name );
    sl->next = keyrings;
    keyrings = sl;

    /* FIXME: We should remove much out of this mpdule and
     * combine it with the keyblock stuff from ringedit.c
     * For now we will simple add the filename as keyblock resource
     */
    rc = add_keyblock_resource( name, 0, 0 );
    if( rc )
	log_error("keyblock resource '%s': %s\n", name, g10_errstr(rc) );
}


/****************
 * Get the name of the keyrings, start with a sequence number of 0.
 */
const char *
get_keyring( int sequence )
{
    STRLIST sl;

    for(sl = keyrings; sl && sequence; sl = sl->next, sequence-- )
	;
    return sl? sl->d : NULL;
}


void
add_secret_keyring( const char *name )
{
    STRLIST sl;
    int rc;

    /* FIXME: check wether this one is available etc */
    /* my be we should do this later */
    sl = m_alloc( sizeof *sl + strlen(name) );
    strcpy(sl->d, name );
    sl->next = secret_keyrings;
    secret_keyrings = sl;

    /* FIXME: We should remove much out of this mpdule and
     * combine it with the keyblock stuff from ringedit.c
     * For now we will simple add the filename as keyblock resource
     */
    rc = add_keyblock_resource( name, 0, 1 );
    if( rc )
	log_error("secret keyblock resource '%s': %s\n", name, g10_errstr(rc) );
}


void
cache_public_cert( PKT_public_cert *pkc )
{
    pkc_cache_entry_t ce;
    u32 keyid[2];

    if( pkc->pubkey_algo == PUBKEY_ALGO_ELGAMAL
	|| pkc->pubkey_algo == PUBKEY_ALGO_RSA ) {
	keyid_from_pkc( pkc, keyid );
    }
    else
	return; /* don't know how to get the keyid */

    for( ce = pkc_cache; ce; ce = ce->next )
	if( ce->keyid[0] == keyid[0] && ce->keyid[1] == keyid[1] ) {
	    if( DBG_CACHE )
		log_debug("cache_public_cert: already in cache\n");
	    return;
	}

    if( pkc_cache_entries > MAX_PKC_CACHE_ENTRIES ) {
	/* FIMXE: use another algorithm to free some cache slots */
	if( pkc_cache_entries == MAX_PKC_CACHE_ENTRIES )  {
	    pkc_cache_entries++;
	    log_info("too many entries in pkc cache - disabled\n");
	}
	ce = pkc_cache;
	free_public_cert( ce->pkc );
    }
    else {
	pkc_cache_entries++;
	ce = m_alloc( sizeof *ce );
	ce->next = pkc_cache;
	pkc_cache = ce;
    }
    ce->pkc = copy_public_cert( NULL, pkc );
    ce->keyid[0] = keyid[0];
    ce->keyid[1] = keyid[1];
}


/****************
 * Store the association of keyid and userid
 */
void
cache_user_id( PKT_user_id *uid, u32 *keyid )
{
    user_id_db_t r;

    for(r=user_id_db; r; r = r->next )
	if( r->keyid[0] == keyid[0] && r->keyid[1] == keyid[1] ) {
	    if( DBG_CACHE )
		log_debug("cache_user_id: already in cache\n");
	    return;
	}

    r = m_alloc( sizeof *r + uid->len-1 );
    r->keyid[0] = keyid[0];
    r->keyid[1] = keyid[1];
    r->len = uid->len;
    memcpy(r->name, uid->name, r->len);
    r->next = user_id_db;
    user_id_db = r;
}



/****************
 * Get a public key and store it into the allocated pkc
 * can be called with PKC set to NULL to just read it into some
 * internal structures.
 */
int
get_pubkey( PKT_public_cert *pkc, u32 *keyid )
{
    keyid_list_t kl;
    int internal = 0;
    int rc = 0;
    pkc_cache_entry_t ce;
    STRLIST sl;


    if( opt.cache_all && !pkc_cache ) {
	log_info("reading all entries ...\n");
	for(sl = keyrings; sl; sl = sl->next )
	    if( !scan_keyring( NULL, NULL, NULL, sl->d ) )
		goto leave;
	log_info("cached %d entries\n", pkc_cache_entries);
    }


    /* lets see wether we checked the keyid already */
    for( kl = unknown_keyids; kl; kl = kl->next )
	if( kl->keyid[0] == keyid[0] && kl->keyid[1] == keyid[1] )
	    return G10ERR_NO_PUBKEY; /* already checked and not found */

    /* 1. Try to get it from our cache */
    for( ce = pkc_cache; ce; ce = ce->next )
	if( ce->keyid[0] == keyid[0] && ce->keyid[1] == keyid[1] ) {
	    if( pkc )
		copy_public_cert( pkc, ce->pkc );
	    return 0;
	}

    /* more init stuff */
    if( !pkc ) {
	pkc = m_alloc_clear( sizeof *pkc );
	internal++;
    }


    /* 2. Try to get it from the keyrings */
    for(sl = keyrings; sl; sl = sl->next )
	if( !scan_keyring( pkc, keyid, NULL, sl->d ) )
	    goto leave;

    /* 3. Try to get it from a key server */

    /* 4. not found: store it for future reference */
    kl = m_alloc( sizeof *kl );
    kl->keyid[0] = keyid[0];
    kl->keyid[1] = keyid[1];
    kl->next = unknown_keyids;
    unknown_keyids = kl;
    rc = G10ERR_NO_PUBKEY;

  leave:
    if( !rc )
	cache_public_cert( pkc );
    if( internal )
	m_free(pkc);
    return rc;
}


/****************
 * Try to get the pubkey by the userid. This functions looks for the
 * first pubkey certificate which has the given name in a user_id.
 * if pkc has the pubkey algo set, the function will only return
 * a pubkey with that algo.
 */
int
get_pubkey_byname( PKT_public_cert *pkc, const char *name )
{
    int internal = 0;
    int rc = 0;
    STRLIST sl;

    if( !pkc ) {
	pkc = m_alloc_clear( sizeof *pkc );
	internal++;
    }

    /* 2. Try to get it from the keyrings */
    for(sl = keyrings; sl; sl = sl->next )
	if( !scan_keyring( pkc, NULL, name, sl->d ) )
	    goto leave;

    /* 3. Try to get it from a key server */

    /* 4. not found: store it for future reference */
    rc = G10ERR_NO_PUBKEY;

  leave:
    if( internal )
	m_free(pkc);
    return rc;
}


/****************
 * Get a secret key and store it into skey
 */
int
get_seckey( PKT_secret_cert *skc, u32 *keyid )
{
    STRLIST sl;
    int rc=0;

    for(sl = secret_keyrings; sl; sl = sl->next )
	if( !(rc=scan_secret_keyring( skc, keyid, NULL, sl->d )) )
	    goto found;
    /* fixme: look at other places */
    goto leave;

  found:
    /* get the secret key (this may prompt for a passprase to
     * unlock the secret key
     */
    if( (rc = check_secret_key( skc )) )
	goto leave;

  leave:
    return rc;
}

/****************
 * Get a secret key by name and store it into skc
 * If NAME is NULL use the default certificate
 */
int
get_seckey_byname( PKT_secret_cert *skc, const char *name, int unprotect )
{
    STRLIST sl;
    int rc=0;

    for(sl = secret_keyrings; sl; sl = sl->next )
	if( !(rc=scan_secret_keyring( skc, NULL, name, sl->d ) ) )
	    goto found;
    /* fixme: look at other places */
    goto leave;

  found:
    /* get the secret key (this may prompt for a passprase to
     * unlock the secret key
     */
    if( unprotect )
	if( (rc = check_secret_key( skc )) )
	    goto leave;

  leave:
    return rc;
}


/****************
 * scan the keyring and look for either the keyid or the name.
 */
static int
scan_keyring( PKT_public_cert *pkc, u32 *keyid,
	      const char *name, const char *filename )
{
    int rc=0;
    int found = 0;
    IOBUF a;
    PACKET pkt;
    int save_mode;
    u32 akeyid[2];
    PKT_public_cert *last_pk = NULL;

    assert( !keyid || !name );

    if( opt.cache_all && (name || keyid) )
	return G10ERR_NO_PUBKEY;

    if( !(a = iobuf_open( filename ) ) ) {
	log_debug("scan_keyring: can't open '%s'\n", filename );
	return G10ERR_KEYRING_OPEN;
    }

    if( !DBG_CACHE )
	;
    else if( name )
	log_debug("scan_keyring %s for '%s'\n",  filename, name );
    else if( keyid )
	log_debug("scan_keyring %s for %08lx %08lx\n",  filename,
						keyid[0], keyid[1] );
    else
	log_debug("scan_keyring %s (all)\n",  filename );

    save_mode = set_packet_list_mode(0);
    init_packet(&pkt);
    while( (rc=parse_packet(a, &pkt)) != -1 ) {
	if( rc )
	    ; /* e.g. unknown packet */
	else if( keyid && found && pkt.pkttype == PKT_PUBLIC_CERT ) {
	    log_error("Hmmm, pubkey without an user id in '%s'\n", filename);
	    goto leave;
	}
	else if( keyid && pkt.pkttype == PKT_PUBLIC_CERT ) {
	    switch( pkt.pkt.public_cert->pubkey_algo ) {
	      case PUBKEY_ALGO_ELGAMAL:
	      case PUBKEY_ALGO_RSA:
		keyid_from_pkc( pkt.pkt.public_cert, akeyid );
		if( akeyid[0] == keyid[0] && akeyid[1] == keyid[1] ) {
		    copy_public_cert( pkc, pkt.pkt.public_cert );
		    found++;
		}
		break;
	      default:
		log_error("cannot handle pubkey algo %d\n",
				     pkt.pkt.public_cert->pubkey_algo);
	    }
	}
	else if( keyid && found && pkt.pkttype == PKT_USER_ID ) {
	    cache_user_id( pkt.pkt.user_id, keyid );
	    goto leave;
	}
	else if( name && pkt.pkttype == PKT_PUBLIC_CERT ) {
	    if( last_pk )
		free_public_cert(last_pk);
	    last_pk = pkt.pkt.public_cert;
	    pkt.pkt.public_cert = NULL;
	}
	else if( name && pkt.pkttype == PKT_USER_ID ) {
	    if( memistr( pkt.pkt.user_id->name, pkt.pkt.user_id->len, name )) {
		if( !last_pk )
		    log_error("Ooops: no pubkey for userid '%.*s'\n",
				pkt.pkt.user_id->len, pkt.pkt.user_id->name);
		else if( pkc->pubkey_algo &&
			 pkc->pubkey_algo != last_pk->pubkey_algo )
		    log_info("skipping id '%.*s': want algo %d, found %d\n",
				pkt.pkt.user_id->len, pkt.pkt.user_id->name,
				pkc->pubkey_algo, last_pk->pubkey_algo );
		else {
		    copy_public_cert( pkc, last_pk );
		    goto leave;
		}
	    }
	}
	else if( !keyid && !name && pkt.pkttype == PKT_PUBLIC_CERT ) {
	    if( last_pk )
		free_public_cert(last_pk);
	    last_pk = pkt.pkt.public_cert;
	    pkt.pkt.public_cert = NULL;
	}
	else if( !keyid && !name && pkt.pkttype == PKT_USER_ID ) {
	    if( !last_pk )
		log_error("Ooops: no pubkey for userid '%.*s'\n",
			    pkt.pkt.user_id->len, pkt.pkt.user_id->name);
	    else {
		if( last_pk->pubkey_algo == PUBKEY_ALGO_ELGAMAL
		    || last_pk->pubkey_algo == PUBKEY_ALGO_RSA ) {
		     keyid_from_pkc( last_pk, akeyid );
		     cache_user_id( pkt.pkt.user_id, akeyid );
		}
		cache_public_cert( last_pk );
	    }
	}
	free_packet(&pkt);
    }
    rc = G10ERR_NO_PUBKEY;

  leave:
    if( last_pk )
	free_public_cert(last_pk);
    free_packet(&pkt);
    iobuf_close(a);
    set_packet_list_mode(save_mode);
    return rc;
}


/****************
 * This is the function to get a secret key. We use an extra function,
 * so that we can easily add special handling for secret keyrings
 * PKT returns the secret key certificate.
 */
static int
scan_secret_keyring( PKT_secret_cert *skc, u32 *keyid,
		     const char *name, const char *filename )
{
    int rc=0;
    int found = 0;
    IOBUF a;
    PACKET pkt;
    int save_mode;
    u32 akeyid[2];
    PKT_secret_cert *last_pk = NULL;
    int get_first;
    u32 dummy_keyid[2];

    get_first = !keyid && !name;
    if( get_first )
	keyid = dummy_keyid;

    if( !(a = iobuf_open( filename ) ) ) {
	log_debug("scan_secret_keyring: can't open '%s'\n", filename );
	return G10ERR_KEYRING_OPEN;
    }

    save_mode = set_packet_list_mode(0);
    init_packet(&pkt);
    while( (rc=parse_packet(a, &pkt)) != -1 ) {
	if( rc )
	    ; /* e.g. unknown packet */
	else if( keyid && found && pkt.pkttype == PKT_SECRET_CERT ) {
	    log_error("Hmmm, seckey without an user id in '%s'\n", filename);
	    goto leave;
	}
	else if( keyid && pkt.pkttype == PKT_SECRET_CERT ) {
	    switch( pkt.pkt.secret_cert->pubkey_algo ) {
	      case PUBKEY_ALGO_ELGAMAL:
	      case PUBKEY_ALGO_RSA:
		if( get_first ) {
		    copy_secret_cert( skc, pkt.pkt.secret_cert );
		    found++;
		}
		else {
		    keyid_from_skc( pkt.pkt.secret_cert, akeyid );
		    if( (akeyid[0] == keyid[0] && akeyid[1] == keyid[1]) ) {
			copy_secret_cert( skc, pkt.pkt.secret_cert );
			found++;
		    }
		}
		break;
	      default:
		log_error("cannot handle pubkey algo %d\n",
				     pkt.pkt.secret_cert->pubkey_algo);
	    }
	}
	else if( keyid && found && pkt.pkttype == PKT_USER_ID ) {
	    goto leave;
	}
	else if( name && pkt.pkttype == PKT_SECRET_CERT ) {
	    if( last_pk )
		free_secret_cert(last_pk);
	    last_pk = pkt.pkt.secret_cert;
	    pkt.pkt.secret_cert = NULL;
	}
	else if( name && pkt.pkttype == PKT_USER_ID ) {
	    if( memistr( pkt.pkt.user_id->name, pkt.pkt.user_id->len, name )) {
		if( !last_pk )
		    log_error("Ooops: no seckey for userid '%.*s'\n",
				pkt.pkt.user_id->len, pkt.pkt.user_id->name);
		else if( skc->pubkey_algo &&
			 skc->pubkey_algo != last_pk->pubkey_algo )
		    log_info("skipping id '%.*s': want algo %d, found %d\n",
				pkt.pkt.user_id->len, pkt.pkt.user_id->name,
				skc->pubkey_algo, last_pk->pubkey_algo );
		else {
		    copy_secret_cert( skc, last_pk );
		    goto leave;
		}
	    }
	}
	else if( !keyid && !name && pkt.pkttype == PKT_SECRET_CERT ) {
	    if( last_pk )
		free_secret_cert(last_pk);
	    last_pk = pkt.pkt.secret_cert;
	    pkt.pkt.secret_cert = NULL;
	}
	else if( !keyid && !name && pkt.pkttype == PKT_USER_ID ) {
	    if( !last_pk )
		log_error("Ooops: no seckey for userid '%.*s'\n",
			    pkt.pkt.user_id->len, pkt.pkt.user_id->name);
	    else {
		if( last_pk->pubkey_algo == PUBKEY_ALGO_ELGAMAL
		   || last_pk->pubkey_algo == PUBKEY_ALGO_RSA ) {
		    keyid_from_skc( last_pk, akeyid );
		    cache_user_id( pkt.pkt.user_id, akeyid );
		}
	    }
	}
	free_packet(&pkt);
    }
    rc = G10ERR_NO_SECKEY;

  leave:
    if( last_pk )
	free_secret_cert(last_pk);
    free_packet(&pkt);
    iobuf_close(a);
    set_packet_list_mode(save_mode);
    return rc;
}


/****************
 * Return a string with a printable representation of the user_id.
 * this string must be freed by m_free.
 */
char*
get_user_id_string( u32 *keyid )
{
    user_id_db_t r;
    char *p;
    int pass=0;
    /* try it two times; second pass reads from keyrings */
    do {
	for(r=user_id_db; r; r = r->next )
	    if( r->keyid[0] == keyid[0] && r->keyid[1] == keyid[1] ) {
		p = m_alloc( r->len + 10 );
		sprintf(p, "%08lX %.*s", keyid[1], r->len, r->name );
		return p;
	    }
    } while( ++pass < 2 && !get_pubkey( NULL, keyid ) );
    p = m_alloc( 15 );
    sprintf(p, "%08lX [?]", keyid[1] );
    return p;
}

char*
get_user_id( u32 *keyid, size_t *rn )
{
    user_id_db_t r;
    char *p;
    int pass=0;
    /* try it two times; second pass reads from keyrings */
    do {
	for(r=user_id_db; r; r = r->next )
	    if( r->keyid[0] == keyid[0] && r->keyid[1] == keyid[1] ) {
		p = m_alloc( r->len );
		memcpy(p, r->name, r->len );
		*rn = r->len;
		return p;
	    }
    } while( ++pass < 2 && !get_pubkey( NULL, keyid ) );
    p = m_alloc( 19 );
    memcpy(p, "[User id not found]", 19 );
    *rn = 19;
    return p;
}


