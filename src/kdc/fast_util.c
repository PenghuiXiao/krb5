/*
 * kdc/fast_util.c
 *
 * Copyright (C) 2009 by the Massachusetts Institute of Technology.
 * All rights reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 * 
 * 
 *
 */

#include <k5-int.h>

#include "kdc_util.h"
#include "extern.h"


/*
 * This function will find the fast and cookie padata and if fast is
 * successfully processed, will throw away (and free) the outer
 * request and update the pointer to point to the inner request.  The
 * checksummed_data points to the data that is in the
 * armored_fast_request checksum; either the pa-tgs-req or the
 * kdc-req-body.
 */

static krb5_error_code armor_ap_request
(struct kdc_request_state *state, krb5_fast_armor *armor)
{
    krb5_error_code retval = 0;
    krb5_auth_context authcontext = NULL;
    krb5_ticket *ticket = NULL;
    krb5_keyblock *subkey = NULL;
    
    assert(armor->armor_type = KRB5_FAST_ARMOR_AP_REQUEST);
    krb5_clear_error_message(kdc_context);
    retval = krb5_auth_con_init(kdc_context, &authcontext);
    if (retval == 0)
	retval = krb5_auth_con_setflags(kdc_context, authcontext, 0); /*disable replay cache*/
    retval = krb5_rd_req(kdc_context, &authcontext,
			 &armor->armor_value, NULL /*server*/,
			 kdc_active_realm->realm_keytab,  NULL, &ticket);
    if (retval !=0) {
	const char * errmsg = krb5_get_error_message(kdc_context, retval);
		krb5_set_error_message(kdc_context, retval,
				       "%s while handling ap-request armor", errmsg);
		krb5_free_error_message(kdc_context, errmsg);
    }
    if (retval == 0) {
	if (!krb5_principal_compare_any_realm(kdc_context,
					      tgs_server,
					      ticket->server)) {
	    krb5_set_error_message(kdc_context, KRB5KDC_ERR_SERVER_NOMATCH,
				   "ap-request armor for something other than  the local TGS");
	    retval = KRB5KDC_ERR_SERVER_NOMATCH;
	}
    }
    if (retval ==0) {
	retval = krb5_auth_con_getrecvsubkey(kdc_context, authcontext, &subkey);
	if (retval !=0 || subkey == NULL) {
	    krb5_set_error_message(kdc_context, KRB5KDC_ERR_POLICY,
				   "ap-request armor without subkey");
	    retval = KRB5KDC_ERR_POLICY;
	}
    }
    if (retval==0) 
	retval = krb5_c_fx_cf2_simple(kdc_context,
				      subkey, "subkeyarmor",
				      ticket->enc_part2->session, "ticketarmor",
				      &state->armor_key);
    if (ticket)
	krb5_free_ticket(kdc_context, ticket);
    if (subkey)
	krb5_free_keyblock(kdc_context, subkey);
    if (authcontext)
	krb5_auth_con_free(kdc_context, authcontext);
    return retval;
}

	
krb5_error_code  kdc_find_fast
(krb5_kdc_req **requestptr,  krb5_data *checksummed_data,
 krb5_keyblock *tgs_subkey,
 struct kdc_request_state *state)
{
    krb5_error_code retval = 0;
    krb5_pa_data *fast_padata, *cookie_padata;
    krb5_data scratch;
    krb5_fast_req * fast_req = NULL;
    krb5_kdc_req *request = *requestptr;
    krb5_fast_armored_req *fast_armored_req = NULL;
    krb5_boolean cksum_valid;

    scratch.data = NULL;
    krb5_clear_error_message(kdc_context);
    fast_padata = find_pa_data(request->padata,
			       KRB5_PADATA_FX_FAST);
    if (fast_padata !=  NULL){
    scratch.length = fast_padata->length;
    scratch.data = (char *) fast_padata->contents;
    retval = decode_krb5_pa_fx_fast_request(&scratch, &fast_armored_req);
    if (retval == 0 &&fast_armored_req->armor) {
	switch (fast_armored_req->armor->armor_type) {
	case KRB5_FAST_ARMOR_AP_REQUEST:
	    retval = armor_ap_request(state, fast_armored_req->armor);
	    break;
	default:
	    krb5_set_error_message(kdc_context, KRB5KDC_ERR_PREAUTH_FAILED,
				   "Unknow FAST armor type %d",
				   fast_armored_req->armor->armor_type);
	    retval = KRB5KDC_ERR_PREAUTH_FAILED;
	}
    }
    if (retval == 0 && !state->armor_key) {
	if (tgs_subkey)
	    retval =krb5_copy_keyblock(kdc_context, tgs_subkey, &state->armor_key);
	else {
	    krb5_set_error_message(kdc_context, KRB5KDC_ERR_PREAUTH_FAILED,
				   "No armor key but FAST armored request present");
	    retval = KRB5KDC_ERR_PREAUTH_FAILED;
	}
    }
    if (retval == 0) {
	krb5_data plaintext;
	plaintext.length = fast_armored_req->enc_part.ciphertext.length;
	plaintext.data = malloc(plaintext.length);
	if (plaintext.data == NULL)
	    retval = ENOMEM;
	retval = krb5_c_decrypt(kdc_context,
				state->armor_key,
				KRB5_KEYUSAGE_FAST_ENC, NULL,
				&fast_armored_req->enc_part,
				&plaintext);
	if (retval == 0)
	    retval = decode_krb5_fast_req(&plaintext, &fast_req);
	if (plaintext.data)
	    free(plaintext.data);
    }
    if (retval == 0)
      retval = krb5_c_verify_checksum(kdc_context, state->armor_key,
				      KRB5_KEYUSAGE_FAST_REQ_CHKSUM,
				      checksummed_data, &fast_armored_req->req_checksum,
				      &cksum_valid);
    if (retval == 0 && !cksum_valid) {
      retval = KRB5KRB_AP_ERR_MODIFIED;
      krb5_set_error_message(kdc_context, KRB5KRB_AP_ERR_MODIFIED,
			     "FAST req_checksum invalid; request modified");
    }
	    if (retval == 0) {
	if ((fast_req->fast_options & UNSUPPORTED_CRITICAL_FAST_OPTIONS) !=0)
	    retval = KRB5KDC_ERR_UNKNOWN_CRITICAL_FAST_OPTION;
    }
	    if (retval == 0)
	        cookie_padata = find_pa_data(fast_req->req_body->padata, KRB5_PADATA_FX_COOKIE);
	    if (retval == 0) {
	      state->fast_options = fast_req->fast_options;
	      if (request->kdc_state == state)
		request->kdc_state = NULL;
	      krb5_free_kdc_req( kdc_context, request);
	      *requestptr = fast_req->req_body;
	      fast_req->req_body = NULL;
	
	    }
    }
    else cookie_padata = find_pa_data(request->padata, KRB5_PADATA_FX_COOKIE);
        if (retval == 0 && cookie_padata != NULL) {
	krb5_pa_data *new_padata = malloc(sizeof (krb5_pa_data));
	if (new_padata != NULL) {
	    retval = ENOMEM;
	} else {
	    new_padata->pa_type = KRB5_PADATA_FX_COOKIE;
	    new_padata->length = cookie_padata->length;
	    new_padata->contents = malloc(new_padata->length);
	    if (new_padata->contents == NULL) {
		retval = ENOMEM;
		free(new_padata);
	    } else {
		memcpy(new_padata->contents, cookie_padata->contents, new_padata->length);
		state->cookie = new_padata;
	    }
	}
    }
	if (fast_req)
	krb5_free_fast_req( kdc_context, fast_req);
    if (fast_armored_req)
	krb5_free_fast_armored_req(kdc_context, fast_armored_req);
    return retval;
}


krb5_error_code kdc_make_rstate(struct kdc_request_state **out)
{
    struct kdc_request_state *state = malloc( sizeof(struct kdc_request_state));
    if (state == NULL)
	return ENOMEM;
    memset( state, 0, sizeof(struct kdc_request_state));
    *out = state;
    return 0;
}

void kdc_free_rstate
(struct kdc_request_state *s)
{
  if (s == NULL)
    return;
    if (s->armor_key)
	krb5_free_keyblock(kdc_context, s->armor_key);
    if (s->reply_key)
	krb5_free_keyblock(kdc_context, s->reply_key);
    if (s->cookie) {
	free(s->cookie->contents);
	free(s->cookie);
    }
    free(s);
}

krb5_error_code kdc_fast_response_handle_padata
(struct kdc_request_state *state,
 krb5_kdc_req *request,
 krb5_kdc_rep *rep)
{
    krb5_error_code retval = 0;
    krb5_fast_finished finish;
    krb5_fast_response fast_response;
    krb5_data *encoded_ticket = NULL;
    krb5_data *encoded_fast_response = NULL;
    krb5_pa_data *pa = NULL, **pa_array;
    krb5_cksumtype cksumtype = CKSUMTYPE_RSA_MD5;
    
    if (!state->armor_key)
	return 0;
    memset(&finish, 0, sizeof(finish));
    fast_response.padata = rep->padata;
    fast_response.rep_key = state->reply_key;
    fast_response.nonce = request->nonce;
    fast_response.finished = &finish;
    finish.client = rep->client;
    pa_array = calloc(3, sizeof(*pa_array));
    if (pa_array == NULL)
	retval = ENOMEM;
    pa = calloc(1, sizeof(krb5_pa_data));
    if (retval == 0 && pa == NULL)
	retval = ENOMEM;
    if (retval == 0)
	retval = krb5_us_timeofday(kdc_context, &finish.timestamp, &finish.usec);
    if (retval == 0)
	retval = encode_krb5_ticket(rep->ticket, &encoded_ticket);
    if (retval == 0)
	retval = krb5_c_make_checksum(kdc_context, cksumtype,
				      state->armor_key, KRB5_KEYUSAGE_FAST_FINISHED,
				      encoded_ticket, &finish.ticket_checksum);
    if (retval == 0)
	retval = encode_krb5_fast_response(&fast_response,  &encoded_fast_response);
    if (retval == 0) {
	pa[0].pa_type = KRB5_PADATA_FX_FAST;
	pa[0].length = encoded_fast_response->length;
	pa[0].contents = (unsigned char *)  encoded_fast_response->data;
	pa_array[0] = &pa[0];
	rep->padata = pa_array;
	pa_array = NULL;
	encoded_fast_response = NULL;
	pa = NULL;
    }
    if (pa)
      free(pa);
    if (encoded_fast_response)
	krb5_free_data(kdc_context, encoded_fast_response);
    if (encoded_ticket)
	krb5_free_data(kdc_context, encoded_ticket);
    if (finish.ticket_checksum.contents)
	krb5_free_checksum_contents(kdc_context, &finish.ticket_checksum);
    return retval;
}

/*
 * We assume the caller is responsible for passing us an in_padata
 * sufficient to include in a FAST error.  In the FAST case we will
 * throw away the e_data in the error (if any); in the non-FAST case
 * we will not use the in_padata.
 */
krb5_error_code kdc_fast_handle_error
(krb5_context context, struct kdc_request_state *state,
 krb5_kdc_req *request,
 krb5_pa_data  **in_padata, krb5_error *err)
{
    krb5_error_code retval = 0;
    krb5_fast_response resp;
    krb5_error fx_error;
    krb5_data *encoded_fx_error = NULL, *encoded_fast_response = NULL;
    krb5_pa_data pa[2];
    krb5_pa_data *outer_pa[3];
    krb5_pa_data **inner_pa = NULL;
    size_t size = 0;
    krb5_data *encoded_e_data = NULL;

    memset(outer_pa, 0, sizeof(outer_pa));
    if (!state->armor_key)
	return 0;
    fx_error = *err;
    fx_error.e_data.data = NULL;
    fx_error.e_data.length = 0;
    for (size = 0; in_padata&&in_padata[size]; size++);
    size +=3;
    inner_pa = calloc(size, sizeof(krb5_pa_data *));
    if (inner_pa == NULL)
	retval = ENOMEM;
    if (retval == 0)
	for (size=0; in_padata&&in_padata[size]; size++)
	    inner_pa[size] = in_padata[size];
    if (retval == 0)
	retval = encode_krb5_error(&fx_error, &encoded_fx_error);
    if (retval == 0) {
	pa[0].pa_type = KRB5_PADATA_FX_ERROR;
	pa[0].length = encoded_fx_error->length;
	pa[0].contents = (unsigned char *) encoded_fx_error->data;
	inner_pa[size++] = &pa[0];
	resp.padata = inner_pa;
	resp.nonce = request->nonce;
	resp.rep_key = NULL;
	resp.finished = NULL;
    }
    if (retval == 0)
	retval = encode_krb5_fast_response(&resp, &encoded_fast_response);
    if (inner_pa)
	free(inner_pa); /*contained storage from caller and our stack*/
    if (retval == 0) {
	pa[0].pa_type = KRB5_PADATA_FX_FAST;
	pa[0].length = encoded_fast_response->length;
	pa[0].contents = (unsigned char *) encoded_fast_response->data;
	outer_pa[0] = &pa[0];
    }
    retval = encode_krb5_padata_sequence(outer_pa, &encoded_e_data);
    if (retval == 0) {
      /*process_as holds onto a pointer to the original e_data and frees it*/
	err->e_data = *encoded_e_data;
	free(encoded_e_data); /*contents belong to err*/
	encoded_e_data = NULL;
    }
    if (encoded_e_data)
	krb5_free_data(kdc_context, encoded_e_data);
    if (encoded_fast_response)
	krb5_free_data(kdc_context, encoded_fast_response);
    if (encoded_fx_error)
	krb5_free_data(kdc_context, encoded_fx_error);
    return retval;
}
