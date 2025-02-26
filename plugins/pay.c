#include "config.h"
#include <bitcoin/chainparams.h>
#include <ccan/array_size/array_size.h>
#include <ccan/asort/asort.h>
#include <ccan/cast/cast.h>
#include <ccan/crypto/siphash24/siphash24.h>
#include <ccan/htable/htable_type.h>
#include <ccan/json_out/json_out.h>
#include <ccan/str/hex/hex.h>
#include <ccan/tal/str/str.h>
#include <common/bolt12_merkle.h>
#include <common/gossmap.h>
#include <common/json_stream.h>
#include <common/json_tok.h>
#include <common/pseudorand.h>
#include <common/type_to_string.h>
#include <plugins/libplugin-pay.h>
#include <stdio.h>

/* Public key of this node. */
static struct node_id my_id;
static unsigned int maxdelay_default;
static bool exp_offers;
static bool disablempp = false;

static LIST_HEAD(pay_status);

static LIST_HEAD(payments);

struct pay_attempt {
	/* What we changed when starting this attempt. */
	const char *why;
	/* Time we started & finished attempt */
	struct timeabs start, end;
	/* Route hint we were using (if any) */
	struct route_info *routehint;
	/* Channels we excluded when doing route lookup. */
	const char **excludes;
	/* Route we got (NULL == route lookup fail). */
	const char *route;
	/* Did we actually try to send a payment? */
	bool sendpay;
	/* The failure result (NULL on success) */
	struct json_out *failure;
	/* The non-failure result (NULL on failure) */
	const char *result;
	/* The blockheight at which the payment attempt was
	 * started.  */
	u32 start_block;
};

struct pay_status {
	/* Destination, as text */
	const char *dest;

	/* We're in 'pay_status' global list. */
	struct list_node list;

	/* Description user provided (if any) */
	const char *label;
	/* Amount they wanted to pay. */
	struct amount_msat msat;
	/* CLTV delay required by destination. */
	u32 final_cltv;
	/* Bolt11/bolt12 invoice. */
	const char *invstring;

	/* What we did about routehints (if anything) */
	const char *routehint_modifications;

	/* Details of shadow route we chose (if any) */
	char *shadow;

	/* Details of initial exclusions (if any) */
	const char *exclusions;

	/* Array of payment attempts. */
	struct pay_attempt *attempts;

	struct sha256 payment_hash;
};

struct pay_command {
	/* Global state */
	struct plugin *plugin;

	/* Destination, as text */
	const char *dest;

	/* How much we're paying, and what riskfactor for routing. */
	struct amount_msat msat;
	/* Blank amount to pay, without fees and shadow route(s). */
	struct amount_msat initial_msat;
	/* riskfactor 12.345% -> riskfactor_millionths = 12345000 */
	u64 riskfactor_millionths;
	unsigned int final_cltv;

	/* Limits on what routes we'll accept. */
	/* 12.345% -> maxfee_pct_millionths = 12345000 */
	u64 maxfee_pct_millionths;
	unsigned int maxdelay;
	struct amount_msat exemptfee;

	/* Payment hash, as text. */
	const char *payment_hash;

	/* Payment secret, if specified by invoice. */
	const char *payment_secret;

	/* Description, if any. */
	const char *label;

	/* Chatty description of attempts. */
	struct pay_status *ps;

	/* Error to use if getroute says it can't find route. */
	const char *expensive_route;

	/* Time to stop retrying. */
	struct timeabs stoptime;

	/* Channels which have failed us. */
	const char **excludes;

	/* Current routehint, if any. */
	struct route_info *current_routehint;

	/* Any remaining routehints to try. */
	struct route_info **routehints;

#if DEVELOPER
	/* Disable the use of shadow route ? */
	double use_shadow;
#endif

	/* Current node during shadow route calculation. */
	const char *shadow_dest;
};

static struct pay_attempt *current_attempt(struct pay_command *pc)
{
	return &pc->ps->attempts[tal_count(pc->ps->attempts)-1];
}

/* Helper to copy JSON object directly into a json_out */
static void json_out_add_raw_len(struct json_out *jout,
				 const char *fieldname,
				 const char *jsonstr, size_t len)
{
	char *p;

	p = json_out_member_direct(jout, fieldname, len);
	memcpy(p, jsonstr, len);
}

static struct json_out *failed_start(struct pay_command *pc)
{
	struct pay_attempt *attempt = current_attempt(pc);

	attempt->end = time_now();
	attempt->failure = json_out_new(pc->ps->attempts);
	json_out_start(attempt->failure, NULL, '{');
	return attempt->failure;
}

static void failed_end(struct json_out *jout)
{
	json_out_end(jout, '}');
	json_out_finished(jout);
}

/* Copy field and member to output, if it exists: return member */
static const jsmntok_t *copy_member(struct json_out *ret,
				    const char *buf, const jsmntok_t *obj,
				    const char *membername)
{
	const jsmntok_t *m = json_get_member(buf, obj, membername);
	if (!m)
		return NULL;

	/* FIXME: The fact it is a string is probably the wrong thing
	 * to handle: if it *is* a string we should probably copy
	 * the quote marks, but json_tok_full/json_tok_full_len
	 * specifically remove those.
	 * It works *now* because it is only used in "code" and
	 * "data": "code" is always numeric, and "data" is usually
	 * a JSON object/key-value table, but pure stromgs will
	 * probably result in invalid JSON.
	 */
	/* Literal copy: it's already JSON escaped, and may be a string. */
	json_out_add_raw_len(ret, membername,
			     json_tok_full(buf, m), json_tok_full_len(m));
	return m;
}

/* Copy (and modify) error object. */
static void attempt_failed_tok(struct pay_command *pc, const char *method,
			       const char *buf, const jsmntok_t *errtok)
{
	const jsmntok_t *msg = json_get_member(buf, errtok, "message");
	struct json_out *failed = failed_start(pc);

	/* Every JSON error response has code and error. */
	copy_member(failed, buf, errtok, "code");
	json_out_add(failed, "message", true,
		     "Call to %s: %.*s",
		     method, msg->end - msg->start,
		     buf + msg->start);
	copy_member(failed, buf, errtok, "data");
	failed_end(failed);
}

static struct command_result *start_pay_attempt(struct command *cmd,
						struct pay_command *pc,
						const char *fmt, ...);

/* Is this (erring) channel within the routehint itself? */
static bool node_or_channel_in_routehint(struct plugin *plugin,
					 const struct route_info *routehint,
					 const char *idstr, size_t idlen)
{
	struct node_id nodeid;
	struct short_channel_id scid;
	bool node_err = true;

	if (!node_id_from_hexstr(idstr, idlen, &nodeid)) {
		if (!short_channel_id_from_str(idstr, idlen, &scid))
			plugin_err(plugin, "bad erring_node or erring_channel '%.*s'",
				   (int)idlen, idstr);
		else
			node_err = false;
	}

	for (size_t i = 0; i < tal_count(routehint); i++) {
		if (node_err) {
			if (node_id_eq(&nodeid, &routehint[i].pubkey))
				return true;
		} else {
			if (short_channel_id_eq(&scid, &routehint[i].short_channel_id))
				return true;
		}
	}
	return false;
}

/* Count times we actually tried to pay, not where route lookup failed or
 * we disliked route for being too expensive, etc. */
static size_t count_sendpays(const struct pay_attempt *attempts)
{
	size_t n = 0;

	for (size_t i = 0; i < tal_count(attempts); i++)
		n += attempts[i].sendpay;

	return n;
}

static struct command_result *waitsendpay_expired(struct command *cmd,
						  struct pay_command *pc)
{
	char *errmsg;
	struct json_stream *data;
	size_t num_attempts = count_sendpays(pc->ps->attempts);

	errmsg = tal_fmt(pc, "Gave up after %zu attempt%s: see paystatus",
			 num_attempts, num_attempts == 1 ? "" : "s");
	data = jsonrpc_stream_fail(cmd, PAY_STOPPED_RETRYING, errmsg);
	json_object_start(data, "data");
	json_array_start(data, "attempts");
	for (size_t i = 0; i < tal_count(pc->ps->attempts); i++) {
		json_object_start(data, NULL);
		if (pc->ps->attempts[i].route)
			json_add_jsonstr(data, "route",
					 pc->ps->attempts[i].route);
		json_out_add_splice(data->jout, "failure",
				    pc->ps->attempts[i].failure);
		json_object_end(data);
	}
	json_array_end(data);
	json_object_end(data);
	return command_finished(cmd, data);
}

static bool routehint_excluded(struct plugin *plugin,
			       const struct route_info *routehint,
			       const char **excludes)
{
	/* Note that we ignore direction here: in theory, we could have
	 * found that one direction of a channel is unavailable, but they
	 * are suggesting we use it the other way.  Very unlikely though! */
	for (size_t i = 0; i < tal_count(excludes); i++)
		if (node_or_channel_in_routehint(plugin,
						 routehint,
						 excludes[i],
						 strlen(excludes[i])))
			return true;
	return false;
}

static struct command_result *next_routehint(struct command *cmd,
					     struct pay_command *pc)
{
	size_t num_attempts = count_sendpays(pc->ps->attempts);

	while (tal_count(pc->routehints) > 0) {
		if (!routehint_excluded(pc->plugin, pc->routehints[0],
					pc->excludes)) {
			pc->current_routehint = pc->routehints[0];
			tal_arr_remove(&pc->routehints, 0);
			return start_pay_attempt(cmd, pc, "Trying route hint");
		}
		tal_free(pc->routehints[0]);
		tal_arr_remove(&pc->routehints, 0);
	}

	/* No (more) routehints; we're out of routes. */
	/* If we eliminated one because it was too pricy, return that. */
	if (pc->expensive_route)
		return command_fail(cmd, PAY_ROUTE_TOO_EXPENSIVE,
				    "%s", pc->expensive_route);

	if (num_attempts > 0)
		return command_fail(cmd, PAY_STOPPED_RETRYING,
				    "Ran out of routes to try after"
				    " %zu attempt%s: see paystatus",
				    num_attempts, num_attempts == 1 ? "" : "s");

	return command_fail(cmd, PAY_ROUTE_NOT_FOUND,
				    "Could not find a route");
}

static struct command_result *
waitblockheight_done(struct command *cmd,
		     const char *buf UNUSED,
		     const jsmntok_t *result UNUSED,
		     struct pay_command *pc)
{
	return start_pay_attempt(cmd, pc,
				 "Retried due to blockheight "
				 "disagreement with payee");
}
static struct command_result *
waitblockheight_error(struct command *cmd,
		      const char *buf UNUSED,
		      const jsmntok_t *error UNUSED,
		      struct pay_command *pc)
{
	if (time_after(time_now(), pc->stoptime))
		return waitsendpay_expired(cmd, pc);
	else
		/* Ehhh just retry it. */
		return waitblockheight_done(cmd, buf, error, pc);
}

static struct command_result *
execute_waitblockheight(struct command *cmd,
			u32 blockheight,
			struct pay_command *pc)
{
	struct out_req *req;
	struct timeabs now = time_now();
	struct timerel remaining;

	if (time_after(now, pc->stoptime))
		return waitsendpay_expired(cmd, pc);

	remaining = time_between(pc->stoptime, now);

	req = jsonrpc_request_start(cmd->plugin, cmd, "waitblockheight",
				    &waitblockheight_done,
				    &waitblockheight_error,
				    pc);
	json_add_u32(req->js, "blockheight", blockheight);
	json_add_u32(req->js, "timeout", time_to_sec(remaining));

	return send_outreq(cmd->plugin, req);
}

/* Gets the remote height from a
 * WIRE_INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS
 * failure.
 * Return 0 if unable to find such a height.
 */
static u32
get_remote_block_height(const char *buf, const jsmntok_t *error)
{
	const u8 *raw_message;
	size_t raw_message_len;
	u16 type;

	/* Is there even a raw_message?  */
	if (json_scan(tmpctx, buf, error, "{data:{raw_message:%}}",
		      JSON_SCAN_TAL(tmpctx, json_tok_bin_from_hex,
				    &raw_message)) != NULL)
		return 0;

	/* BOLT #4:
	 *
	 * 1. type: PERM|15 (`incorrect_or_unknown_payment_details`)
	 * 2. data:
   	 * * [`u64`:`htlc_msat`]
   	 * * [`u32`:`height`]
	 *
	 */
	raw_message_len = tal_count(raw_message);

	type = fromwire_u16(&raw_message, &raw_message_len); /* type */
	if (type != WIRE_INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS)
		return 0;

	(void) fromwire_u64(&raw_message, &raw_message_len); /* htlc_msat */

	return fromwire_u32(&raw_message, &raw_message_len); /* height */
}

static struct command_result *waitsendpay_error(struct command *cmd,
						const char *buf,
						const jsmntok_t *error,
						struct pay_command *pc)
{
	struct pay_attempt *attempt = current_attempt(pc);
	errcode_t code;
	int failcode;
	const char *err;

	attempt_failed_tok(pc, "waitsendpay", buf, error);

	err = json_scan(tmpctx, buf, error, "{code:%}",
			JSON_SCAN(json_to_errcode, &code));
	if (err)
		plugin_err(cmd->plugin, "waitsendpay error %s? '%.*s'",
			   err,
			   json_tok_full_len(error), json_tok_full(buf, error));

	if (code != PAY_UNPARSEABLE_ONION) {
		err = json_scan(tmpctx, buf, error, "{data:{failcode:%}}",
				JSON_SCAN(json_to_int, &failcode));
		if (err)
			plugin_err(cmd->plugin, "waitsendpay failcode error %s '%.*s'",
				   err,
				   json_tok_full_len(error), json_tok_full(buf, error));
	}

	/* Special case for WIRE_INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS.
	 *
	 * One possible trigger for this failure is that the receiver
	 * thinks the final timeout it gets is too near the future.
	 *
	 * For the most part, we respect the indicated `final_cltv`
	 * in the invoice, and our shadow routing feature also tends
	 * to give more timing budget to the receiver than the
	 * `final_cltv`.
	 *
	 * However, there is an edge case possible on real networks:
	 *
	 * * We send out a payment respecting the `final_cltv` of
	 *   the receiver.
	 * * Miners mine a new block while the payment is in transit.
	 * * By the time the payment reaches the receiver, the
	 *   payment violates the `final_cltv` because the receiver
	 *   is now using a different basis blockheight.
	 *
	 * This is a transient error.
	 * Unfortunately, WIRE_INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS
	 * is marked with the PERM bit.
	 * This means that we would give up on this since `waitsendpay`
	 * would return PAY_DESTINATION_PERM_FAIL instead of
	 * PAY_TRY_OTHER_ROUTE.
	 * Thus the `pay` plugin would not retry this case.
	 *
	 * Thus, we need to add this special-case checking here, where
	 * the blockheight when we started the pay attempt was not
	 * the same as what the payee reports.
	 *
	 * In the past this particular failure had its own failure code,
	 * equivalent to 17.
	 * In case the receiver is a really old software, we also
	 * special-case it here.
	 */
	if ((code != PAY_UNPARSEABLE_ONION) &&
	    ((failcode == 17) ||
	     ((failcode == WIRE_INCORRECT_OR_UNKNOWN_PAYMENT_DETAILS) &&
	      (attempt->start_block < get_remote_block_height(buf, error))))) {
		u32 target_blockheight;

		if (failcode == 17)
			target_blockheight = attempt->start_block + 1;
		else
			target_blockheight = get_remote_block_height(buf, error);

		return execute_waitblockheight(cmd, target_blockheight,
					       pc);
	}

	/* FIXME: Handle PAY_UNPARSEABLE_ONION! */

	/* Many error codes are final. */
	if (code != PAY_TRY_OTHER_ROUTE) {
		return forward_error(cmd, buf, error, pc);
	}

	if (time_after(time_now(), pc->stoptime)) {
		return waitsendpay_expired(cmd, pc);
	}

	if (failcode & NODE) {
		struct node_id id;
		const char *idstr, *err;

		err = json_scan(tmpctx, buf, error, "{data:{erring_node:%}}",
				JSON_SCAN(json_to_node_id, &id));
		if (err)
			plugin_err(cmd->plugin, "waitsendpay error %s '%.*s'",
				   err,
				   json_tok_full_len(error),
				   json_tok_full(buf, error));

		/* FIXME: Keep as node_id, don't use strings. */
		idstr = node_id_to_hexstr(tmpctx, &id);

		/* If failure is in routehint part, try next one */
		if (node_or_channel_in_routehint(pc->plugin, pc->current_routehint,
						 idstr, strlen(idstr)))
			return next_routehint(cmd, pc);

		/* Otherwise, add erring channel to exclusion list. */
		tal_arr_expand(&pc->excludes, tal_steal(pc->excludes, idstr));
	} else {
		struct short_channel_id scid;
		u32 dir;
		const char *scidstr, *err;

		err = json_scan(tmpctx, buf, error,
				"{data:{erring_channel:%,erring_direction:%}}",
				JSON_SCAN(json_to_short_channel_id, &scid),
				JSON_SCAN(json_to_number, &dir));
		if (err)
			plugin_err(cmd->plugin, "waitsendpay error %s '%.*s'",
				   err,
				   json_tok_full_len(error),
				   json_tok_full(buf, error));

		scidstr = short_channel_id_to_str(tmpctx, &scid);
		/* If failure is in routehint part, try next one */
		if (node_or_channel_in_routehint(pc->plugin, pc->current_routehint,
						 scidstr, strlen(scidstr)))
			return next_routehint(cmd, pc);

		/* Otherwise, add erring channel to exclusion list. */
		tal_arr_expand(&pc->excludes,
			       tal_fmt(pc->excludes, "%s/%u", scidstr, dir));
	}

	/* Try again. */
	return start_pay_attempt(cmd, pc, "Excluded %s %s",
				 failcode & NODE ? "node" : "channel",
				 pc->excludes[tal_count(pc->excludes)-1]);
}

static struct command_result *waitsendpay_done(struct command *cmd,
					       const char *buf,
					       const jsmntok_t *result,
					       struct pay_command *pc)
{
	struct pay_attempt *attempt = current_attempt(pc);

	attempt->result = json_strdup(pc->ps->attempts, buf, result);
	attempt->end = time_now();

	return forward_result(cmd, buf, result, pc);
}

static struct command_result *sendpay_done(struct command *cmd,
					   const char *buf,
					   const jsmntok_t *result,
					   struct pay_command *pc)
{
	struct out_req *req = jsonrpc_request_start(cmd->plugin, cmd,
						    "waitsendpay",
						    waitsendpay_done,
						    waitsendpay_error, pc);
	json_add_string(req->js, "payment_hash", pc->payment_hash);

	return send_outreq(cmd->plugin, req);
}

/* Calculate how many millisatoshi we need at the start of this route
 * to get msatoshi to the end. */
static bool route_msatoshi(struct amount_msat *total,
			   const struct amount_msat msat,
			   const struct route_info *route, size_t num_route)
{
	*total = msat;
	for (ssize_t i = num_route - 1; i >= 0; i--) {
		if (!amount_msat_add_fee(total,
					 route[i].fee_base_msat,
					 route[i].fee_proportional_millionths))
			return false;
	}
	return true;
}

/* Calculate cltv we need at the start of this route to get cltv at the end. */
static u32 route_cltv(u32 cltv,
		      const struct route_info *route, size_t num_route)
{
	for (size_t i = 0; i < num_route; i++)
		cltv += route[i].cltv_expiry_delta;
	return cltv;
}

/* The pubkey to use is the destination of this routehint. */
static const char *route_pubkey(const tal_t *ctx,
				const struct pay_command *pc,
				const struct route_info *routehint,
				size_t n)
{
	if (n == tal_count(routehint))
		return pc->dest;
	return type_to_string(ctx, struct node_id, &routehint[n].pubkey);
}

static const char *join_routehint(const tal_t *ctx,
				  const char *buf,
				  const jsmntok_t *route,
				  const struct pay_command *pc,
				  const struct route_info *routehint)
{
	char *ret;

	/* Truncate closing ] from route */
	ret = tal_strndup(ctx, buf + route->start, route->end - route->start - 1);
	for (size_t i = 0; i < tal_count(routehint); i++) {
		/* amount to be received by *destination* */
		struct amount_msat dest_amount;

		if (!route_msatoshi(&dest_amount, pc->msat,
				    routehint + i + 1,
				    tal_count(routehint) - i - 1))
			return tal_free(ret);

		tal_append_fmt(&ret, ", {"
			       " \"id\": \"%s\","
			       " \"channel\": \"%s\","
			       " \"msatoshi\": \"%s\","
			       " \"delay\": %u }",
			       /* pubkey of *destination* */
			       route_pubkey(tmpctx, pc, routehint, i + 1),
			       type_to_string(tmpctx, struct short_channel_id,
					      &routehint[i].short_channel_id),
			       type_to_string(tmpctx, struct amount_msat,
					      &dest_amount),
			       /* cltv for *destination* */
			       route_cltv(pc->final_cltv, routehint + i + 1,
					  tal_count(routehint) - i - 1));
	}
	/* Put ] back */
	tal_append_fmt(&ret, "]");
	return ret;
}

static struct command_result *sendpay_error(struct command *cmd,
					    const char *buf,
					    const jsmntok_t *error,
					    struct pay_command *pc)
{
	attempt_failed_tok(pc, "sendpay", buf, error);

	return forward_error(cmd, buf, error, pc);
}

static const jsmntok_t *find_worst_channel(const char *buf,
					   const jsmntok_t *route,
					   const char *fieldname)
{
	u64 prev, worstval = 0;
	const jsmntok_t *worst = NULL, *t, *t_prev = NULL;
	size_t i;

	json_for_each_arr(i, t, route) {
		u64 val;

		json_to_u64(buf, json_get_member(buf, t, fieldname), &val);

		/* For the first hop, now we can't know if it's the worst.
		 * Just store the info and continue. */
		if (!i) {
			prev = val;
			t_prev = t;
			continue;
		}

		if (worst == NULL || prev - val > worstval) {
			worst = t_prev;
			worstval = prev - val;
		}
		prev = val;
		t_prev = t;
	}

	return worst;
}

/* Can't exclude if it's in routehint itself. */
static bool maybe_exclude(struct pay_command *pc,
			  const char *buf, const jsmntok_t *route)
{
	const jsmntok_t *scid, *dir;

	if (!route)
		return false;

	scid = json_get_member(buf, route, "channel");

	if (node_or_channel_in_routehint(pc->plugin,
					 pc->current_routehint,
					 buf + scid->start,
					 scid->end - scid->start))
		return false;

	dir = json_get_member(buf, route, "direction");
	tal_arr_expand(&pc->excludes,
		       tal_fmt(pc->excludes, "%.*s/%c",
			       scid->end - scid->start,
			       buf + scid->start,
			       buf[dir->start]));
	return true;
}

static struct command_result *getroute_done(struct command *cmd,
					    const char *buf,
					    const jsmntok_t *result,
					    struct pay_command *pc)
{
	struct pay_attempt *attempt = current_attempt(pc);
	const jsmntok_t *t = json_get_member(buf, result, "route");
	struct amount_msat fee;
	struct amount_msat max_fee;
	u32 delay;
	struct out_req *req;
	const char *err;

	if (!t)
		plugin_err(cmd->plugin, "getroute gave no 'route'? '%.*s'",
			   result->end - result->start, buf);

	if (pc->current_routehint) {
		attempt->route = join_routehint(pc->ps->attempts, buf, t,
						pc, pc->current_routehint);
		if (!attempt->route) {
			struct json_out *failed = failed_start(pc);
			json_out_add(failed, "message", true,
				     "Joining routehint gave absurd fee");
			failed_end(failed);
			return next_routehint(cmd, pc);
		}
	} else
		attempt->route = json_strdup(pc->ps->attempts, buf, t);

	err = json_scan(tmpctx, buf, t, "[0:{msatoshi:%,delay:%}]",
			JSON_SCAN(json_to_msat, &fee),
			JSON_SCAN(json_to_number, &delay));
	if (err)
		plugin_err(cmd->plugin, "getroute %s? %.*s",
			   err,
			   json_tok_full_len(result), json_tok_full(buf, result));

	if (pc->maxfee_pct_millionths / 100 > UINT32_MAX)
		plugin_err(cmd->plugin, "max fee percent too large: %lf",
			   pc->maxfee_pct_millionths / 1000000.0);

	if (!amount_msat_fee(&max_fee, pc->msat, 0,
			     (u32)(pc->maxfee_pct_millionths / 100)))
		plugin_err(
		    cmd->plugin, "max fee too large: %s * %lf%%",
		    type_to_string(tmpctx, struct amount_msat, &pc->msat),
		    pc->maxfee_pct_millionths / 1000000.0);

	if (amount_msat_greater(fee, pc->exemptfee) &&
	    amount_msat_greater(fee, max_fee)) {
		const jsmntok_t *charger;
		struct json_out *failed;
		char *feemsg;

		feemsg = tal_fmt(pc, "Route wanted fee of %s",
				 type_to_string(tmpctx, struct amount_msat,
						&fee));
		failed = failed_start(pc);
		json_out_addstr(failed, "message", feemsg);
		failed_end(failed);

		/* Remember this if we eliminating this causes us to have no
		 * routes at all! */
		if (!pc->expensive_route)
			pc->expensive_route = feemsg;
		else
			tal_free(feemsg);

		/* Try excluding most fee-charging channel (unless it's in
		 * routeboost). */
		charger = find_worst_channel(buf, t, "msatoshi");
		if (maybe_exclude(pc, buf, charger)) {
			return start_pay_attempt(cmd, pc,
						 "Excluded expensive channel %s",
						 pc->excludes[tal_count(pc->excludes)-1]);
		}

		return next_routehint(cmd, pc);
	}

	if (delay > pc->maxdelay) {
		const jsmntok_t *delayer;
		struct json_out *failed;
		char *feemsg;

		feemsg = tal_fmt(pc, "Route wanted delay of %u blocks", delay);
		failed = failed_start(pc);
		json_out_addstr(failed, "message", feemsg);
		failed_end(failed);

		/* Remember this if we eliminating this causes us to have no
		 * routes at all! */
		if (!pc->expensive_route)
			pc->expensive_route = feemsg;
		else
			tal_free(failed);

		delayer = find_worst_channel(buf, t, "delay");

		/* Try excluding most delaying channel (unless it's in
		 * routeboost). */
		if (maybe_exclude(pc, buf, delayer)) {
			return start_pay_attempt(cmd, pc,
						 "Excluded delaying channel %s",
						 pc->excludes[tal_count(pc->excludes)-1]);
		}

		return next_routehint(cmd, pc);
	}

	attempt->sendpay = true;
	req = jsonrpc_request_start(cmd->plugin, cmd, "sendpay",
				    sendpay_done, sendpay_error, pc);
	json_add_jsonstr(req->js, "route", attempt->route);
	json_add_string(req->js, "payment_hash", pc->payment_hash);
	json_add_string(req->js, "bolt11", pc->ps->invstring);
	if (pc->label)
		json_add_string(req->js, "label", pc->label);
	if (pc->payment_secret)
		json_add_string(req->js, "payment_secret", pc->payment_secret);

	return send_outreq(cmd->plugin, req);
}

static struct command_result *getroute_error(struct command *cmd,
					     const char *buf,
					     const jsmntok_t *error,
					     struct pay_command *pc)
{
	errcode_t code;
	const jsmntok_t *codetok;

	attempt_failed_tok(pc, "getroute", buf, error);

	codetok = json_get_member(buf, error, "code");
	if (!json_to_errcode(buf, codetok, &code))
		plugin_err(cmd->plugin, "getroute error gave no 'code'? '%.*s'",
			   error->end - error->start, buf + error->start);

	/* Strange errors from getroute should be forwarded. */
	if (code != PAY_ROUTE_NOT_FOUND)
		return forward_error(cmd, buf, error, pc);

	return next_routehint(cmd, pc);
}

/* Deep copy of excludes array. */
static const char **dup_excludes(const tal_t *ctx, const char **excludes)
{
	const char **ret = tal_dup_talarr(ctx, const char *, excludes);
	for (size_t i = 0; i < tal_count(ret); i++)
		ret[i] = tal_strdup(ret, excludes[i]);
	return ret;
}

/* Get a route from the lightningd. */
static struct command_result *execute_getroute(struct command *cmd,
					       struct pay_command *pc)
{
	struct pay_attempt *attempt = current_attempt(pc);

	u32 max_hops = ROUTING_MAX_HOPS;
	struct amount_msat msat;
	const char *dest;
	u32 cltv;
	struct out_req *req;

	/* routehint set below. */

	/* If we have a routehint, try that first; we need to do extra
	 * checks that it meets our criteria though. */
	if (pc->current_routehint) {
		attempt->routehint = tal_steal(pc->ps, pc->current_routehint);
		if (!route_msatoshi(&msat, pc->msat,
				    attempt->routehint,
				    tal_count(attempt->routehint))) {
			struct json_out *failed;

			failed = failed_start(pc);
			json_out_addstr(failed, "message",
					"Routehint absurd fee");
			failed_end(failed);
			return next_routehint(cmd, pc);
		}
		dest = type_to_string(tmpctx, struct node_id,
				      &attempt->routehint[0].pubkey);
		max_hops -= tal_count(attempt->routehint);
		cltv = route_cltv(pc->final_cltv,
				  attempt->routehint,
				  tal_count(attempt->routehint));
	} else {
		msat = pc->msat;
		dest = pc->dest;
		cltv = pc->final_cltv;
		attempt->routehint = NULL;
	}

	/* OK, ask for route to destination */
	req = jsonrpc_request_start(cmd->plugin, cmd, "getroute",
				    getroute_done, getroute_error, pc);
	json_add_string(req->js, "id", dest);
	json_add_string(req->js, "msatoshi",
			type_to_string(tmpctx, struct amount_msat, &msat));
	json_add_u32(req->js, "cltv", cltv);
	json_add_u32(req->js, "maxhops", max_hops);
	json_add_member(req->js, "riskfactor", false, "%lf",
			pc->riskfactor_millionths / 1000000.0);
	if (tal_count(pc->excludes) != 0) {
		json_array_start(req->js, "exclude");
		for (size_t i = 0; i < tal_count(pc->excludes); i++)
			json_add_string(req->js, NULL, pc->excludes[i]);
		json_array_end(req->js);
	}

	return send_outreq(cmd->plugin, req);
}

static struct command_result *
getstartblockheight_done(struct command *cmd,
			 const char *buf,
			 const jsmntok_t *result,
			 struct pay_command *pc)
{
	const jsmntok_t *blockheight_tok;
	u32 blockheight;

	blockheight_tok = json_get_member(buf, result, "blockheight");
	if (!blockheight_tok)
		plugin_err(cmd->plugin, "getstartblockheight: "
			   "getinfo gave no 'blockheight'? '%.*s'",
			   result->end - result->start, buf);

	if (!json_to_u32(buf, blockheight_tok, &blockheight))
		plugin_err(cmd->plugin, "getstartblockheight: "
			   "getinfo gave non-unsigned-32-bit 'blockheight'? '%.*s'",
			   result->end - result->start, buf);

	current_attempt(pc)->start_block = blockheight;

	return execute_getroute(cmd, pc);
}

static struct command_result *
getstartblockheight_error(struct command *cmd,
			  const char *buf,
			  const jsmntok_t *error,
			  struct pay_command *pc)
{
	/* Should never happen.  */
	plugin_err(cmd->plugin, "getstartblockheight: getinfo failed!? '%.*s'",
		   error->end - error->start, buf);
}

static struct command_result *
execute_getstartblockheight(struct command *cmd,
			    struct pay_command *pc)
{
	struct out_req *req = jsonrpc_request_start(cmd->plugin, cmd, "getinfo",
						    &getstartblockheight_done,
						    &getstartblockheight_error,
						    pc);
	return send_outreq(cmd->plugin, req);
}

static struct command_result *start_pay_attempt(struct command *cmd,
						struct pay_command *pc,
						const char *fmt, ...)
{
	struct pay_attempt *attempt;
	va_list ap;
	size_t n;

	n = tal_count(pc->ps->attempts);
	tal_resize(&pc->ps->attempts, n+1);
	attempt = &pc->ps->attempts[n];

	va_start(ap, fmt);
	attempt->start = time_now();
	/* Mark it unfinished */
	attempt->end.ts.tv_sec = -1;
	attempt->excludes = dup_excludes(pc->ps, pc->excludes);
	attempt->route = NULL;
	attempt->failure = NULL;
	attempt->result = NULL;
	attempt->sendpay = false;
	attempt->why = tal_vfmt(pc->ps, fmt, ap);
	va_end(ap);

	return execute_getstartblockheight(cmd, pc);
}

/* BOLT #7:
 *
 * If a route is computed by simply routing to the intended recipient and
 * summing the `cltv_expiry_delta`s, then it's possible for intermediate nodes
 * to guess their position in the route. Knowing the CLTV of the HTLC, the
 * surrounding network topology, and the `cltv_expiry_delta`s gives an
 * attacker a way to guess the intended recipient. Therefore, it's highly
 * desirable to add a random offset to the CLTV that the intended recipient
 * will receive, which bumps all CLTVs along the route.
 *
 * In order to create a plausible offset, the origin node MAY start a limited
 * random walk on the graph, starting from the intended recipient and summing
 * the `cltv_expiry_delta`s, and use the resulting sum as the offset.  This
 * effectively creates a _shadow route extension_ to the actual route and
 * provides better protection against this attack vector than simply picking a
 * random offset would.
 */
static struct command_result *shadow_route(struct command *cmd,
					   struct pay_command *pc);

static struct command_result *add_shadow_route(struct command *cmd,
					       const char *buf,
					       const jsmntok_t *result,
					       struct pay_command *pc)
{
	/* Use reservoir sampling across the capable channels. */
	const jsmntok_t *channels = json_get_member(buf, result, "channels");
	const jsmntok_t *chan, *best = NULL;
	size_t i;
	u64 sample = 0;
	struct route_info *route = tal_arr(NULL, struct route_info, 1);
	struct amount_msat fees, maxfees;
	/* Don't go above this. Note how we use the initial amount to get the percentage
	 * of the fees, or it would increase with the addition of new shadow routes. */
	if (!amount_msat_fee(&maxfees, pc->initial_msat, 0, pc->maxfee_pct_millionths))
		plugin_err(cmd->plugin, "Overflow when computing maxfees for "
					"shadow routes.");

	json_for_each_arr(i, chan, channels) {
		u64 v = pseudorand(UINT64_MAX);

		if (!best || v > sample) {
			struct amount_sat sat;

			json_to_sat(buf, json_get_member(buf, chan, "satoshis"), &sat);
			if (amount_msat_greater_sat(pc->msat, sat))
				continue;

			/* Don't use if total would exceed 1/4 of our time allowance. */
			json_to_u16(buf, json_get_member(buf, chan, "delay"),
			            &route[0].cltv_expiry_delta);
			if ((pc->final_cltv + route[0].cltv_expiry_delta) * 4 > pc->maxdelay)
				continue;

			json_to_number(buf, json_get_member(buf, chan, "base_fee_millisatoshi"),
			               &route[0].fee_base_msat);
			json_to_number(buf, json_get_member(buf, chan, "fee_per_millionth"),
			               &route[0].fee_proportional_millionths);

			if (!amount_msat_fee(&fees, pc->initial_msat, route[0].fee_base_msat,
					    route[0].fee_proportional_millionths)
			    || amount_msat_greater_eq(fees, maxfees))
				continue;

			best = chan;
			sample = v;
		}
	}

	if (!best) {
		tal_append_fmt(&pc->ps->shadow,
			       "No suitable channels found to %s. ",
			       pc->shadow_dest);
		return start_pay_attempt(cmd, pc, "Initial attempt");
	}

	pc->final_cltv += route[0].cltv_expiry_delta;
	pc->shadow_dest = json_strdup(pc, buf,
				      json_get_member(buf, best, "destination"));
	route_msatoshi(&pc->msat, pc->msat, route, 1);
	tal_append_fmt(&pc->ps->shadow,
		       "Added %u cltv delay, %u base fee, and %u ppm fee "
		       "for shadow to %s.",
		       route[0].cltv_expiry_delta, route[0].fee_base_msat,
		       route[0].fee_proportional_millionths,
		       pc->shadow_dest);
	tal_free(route);

	return shadow_route(cmd, pc);
}

static struct command_result *shadow_route(struct command *cmd,
					   struct pay_command *pc)
{
	struct out_req *req;

#if DEVELOPER
	if (!pc->use_shadow)
		return start_pay_attempt(cmd, pc, "Initial attempt");
#endif
	if (pseudorand(2) == 0)
		return start_pay_attempt(cmd, pc, "Initial attempt");

	req = jsonrpc_request_start(cmd->plugin, cmd, "listchannels",
				    add_shadow_route, forward_error, pc);
	json_add_string(req->js, "source", pc->shadow_dest);
	return send_outreq(cmd->plugin, req);
}

/* gossipd doesn't know much about the current state of channels; here we
 * manually exclude peers which are disconnected and channels which lack
 * current capacity (it will eliminate those without total capacity). */
static struct command_result *listpeers_done(struct command *cmd,
					     const char *buf,
					     const jsmntok_t *result,
					     struct pay_command *pc)
{
	const jsmntok_t *peers, *peer;
	size_t i;
	char *mods = tal_strdup(tmpctx, "");

	peers = json_get_member(buf, result, "peers");
	if (!peers)
		plugin_err(cmd->plugin, "listpeers gave no 'peers'? '%.*s'",
			   result->end - result->start, buf);

	json_for_each_arr(i, peer, peers) {
		const jsmntok_t *chans, *chan;
		bool connected;
		size_t j;

		json_to_bool(buf, json_get_member(buf, peer, "connected"),
			     &connected);
		chans = json_get_member(buf, peer, "channels");
		json_for_each_arr(j, chan, chans) {
			const jsmntok_t *state, *scid, *dir;
			struct amount_msat spendable;

			/* gossipd will only consider things in state NORMAL
			 * anyway; we don't need to exclude others. */
			state = json_get_member(buf, chan, "state");
			if (!json_tok_streq(buf, state, "CHANNELD_NORMAL"))
				continue;

			json_to_msat(buf,
				    json_get_member(buf, chan,
						    "spendable_msatoshi"),
				    &spendable);

			if (connected
			    && amount_msat_greater_eq(spendable, pc->msat))
				continue;

			/* Exclude this disconnected or low-capacity channel */
			scid = json_get_member(buf, chan, "short_channel_id");
			dir = json_get_member(buf, chan, "direction");
			tal_arr_expand(&pc->excludes,
				       tal_fmt(pc->excludes, "%.*s/%c",
					       scid->end - scid->start,
					       buf + scid->start,
					       buf[dir->start]));

			tal_append_fmt(&mods,
				       "Excluded channel %s (%s, %s). ",
				       pc->excludes[tal_count(pc->excludes)-1],
				       type_to_string(tmpctx, struct amount_msat,
						      &spendable),
				       connected ? "connected" : "disconnected");
		}
	}

	if (!streq(mods, ""))
		pc->ps->exclusions = tal_steal(pc->ps, mods);

	pc->ps->shadow = tal_strdup(pc->ps, "");
	return shadow_route(cmd, pc);
}

/* Trim route to this length by taking from the *front* of route
 * (end points to destination, so we need that bit!) */
static void trim_route(struct route_info **route, size_t n)
{
	size_t remove = tal_count(*route) - n;
	memmove(*route, *route + remove, sizeof(**route) * n);
	tal_resize(route, n);
}

/* Make sure routehints are reasonable length, and (since we assume we
 * can append), not directly to us.  Note: untrusted data! */
static struct route_info **filter_routehints(struct pay_command *pc,
					     struct route_info **hints)
{
	char *mods = tal_strdup(tmpctx, "");

	for (size_t i = 0; i < tal_count(hints); i++) {
		/* Trim any routehint > 10 hops */
		size_t max_hops = ROUTING_MAX_HOPS / 2;
		if (tal_count(hints[i]) > max_hops) {
			tal_append_fmt(&mods,
				       "Trimmed routehint %zu (%zu hops) to %zu. ",
				       i, tal_count(hints[i]), max_hops);
			trim_route(&hints[i], max_hops);
		}

		/* If we are first hop, trim. */
		if (tal_count(hints[i]) > 0
		    && node_id_eq(&hints[i][0].pubkey, &my_id)) {
			tal_append_fmt(&mods,
				       "Removed ourselves from routehint %zu. ",
				       i);
			trim_route(&hints[i], tal_count(hints[i])-1);
		}

		/* If route is empty, remove altogether. */
		if (tal_count(hints[i]) == 0) {
			tal_append_fmt(&mods,
				       "Removed empty routehint %zu. ", i);
			tal_arr_remove(&hints, i);
			i--;
		}
	}

	if (!streq(mods, ""))
		pc->ps->routehint_modifications = tal_steal(pc->ps, mods);

	return tal_steal(pc, hints);
}

static struct pay_status *add_pay_status(struct pay_command *pc,
					 const char *invstring STEALS)
{
	struct pay_status *ps = tal(NULL, struct pay_status);

	/* The pay_status outlives the pc, so it simply takes field ownership */
	ps->dest = tal_steal(ps, pc->dest);
	ps->label = tal_steal(ps, pc->label);
	ps->msat = pc->msat;
	ps->final_cltv = pc->final_cltv;
	ps->invstring = tal_steal(ps, invstring);
	ps->routehint_modifications = NULL;
	ps->shadow = NULL;
	ps->exclusions = NULL;
	ps->attempts = tal_arr(ps, struct pay_attempt, 0);
	hex_decode(pc->payment_hash, strlen(pc->payment_hash),
		   &ps->payment_hash, sizeof(ps->payment_hash));

	list_add_tail(&pay_status, &ps->list);
	return ps;
}

#ifndef COMPAT_V090
UNUSED
#endif
static struct command_result *json_pay(struct command *cmd,
				       const char *buf,
				       const jsmntok_t *params)
{
	struct amount_msat *msat;
	struct bolt11 *b11;
	const char *b11str;
	char *fail;
	u64 *riskfactor_millionths;
	unsigned int *retryfor;
	struct pay_command *pc = tal(cmd, struct pay_command);
	u64 *maxfee_pct_millionths;
	unsigned int *maxdelay;
	struct amount_msat *exemptfee;
	struct out_req *req;
#if DEVELOPER
	bool *use_shadow;
#endif

	if (!param(cmd, buf, params, p_req("bolt11", param_string, &b11str),
		   p_opt("msatoshi", param_msat, &msat),
		   p_opt("label", param_string, &pc->label),
		   p_opt_def("riskfactor", param_millionths,
			     &riskfactor_millionths, 10000000),
		   p_opt_def("maxfeepercent", param_millionths,
			     &maxfee_pct_millionths, 500000),
		   p_opt_def("retry_for", param_number, &retryfor, 60),
		   p_opt_def("maxdelay", param_number, &maxdelay,
			     maxdelay_default),
		   p_opt_def("exemptfee", param_msat, &exemptfee,
			     AMOUNT_MSAT(5000)),
#if DEVELOPER
		   p_opt_def("use_shadow", param_bool, &use_shadow, true),
#endif
		   NULL))
		return command_param_failed();

	b11 = bolt11_decode(cmd, b11str, plugin_feature_set(cmd->plugin),
			    NULL, chainparams, &fail);
	if (!b11) {
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Invalid bolt11: %s", fail);
	}

	if (!b11->chain) {
		return command_fail(cmd, PAY_ROUTE_NOT_FOUND, "Invoice is for an unknown network");
	}

	if (b11->chain != chainparams) {
		return command_fail(cmd, PAY_ROUTE_NOT_FOUND, "Invoice is for another network %s", b11->chain->network_name);
	}

	if (time_now().ts.tv_sec > b11->timestamp + b11->expiry) {
		return command_fail(cmd, PAY_INVOICE_EXPIRED, "Invoice expired");
	}

	if (b11->msat) {
		if (msat) {
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "msatoshi parameter unnecessary");
		}
		pc->msat = pc->initial_msat = *b11->msat;
	} else {
		if (!msat) {
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "msatoshi parameter required");
		}
		pc->msat = pc->initial_msat = *msat;
	}

	/* Sanity check */
	if (feature_offered(b11->features, OPT_VAR_ONION)
	    && !b11->payment_secret) {
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Invalid bolt11:"
				    " sets feature var_onion with no secret");
	}

	pc->maxfee_pct_millionths = *maxfee_pct_millionths;
	pc->maxdelay = *maxdelay;
	pc->exemptfee = *exemptfee;
	pc->riskfactor_millionths = *riskfactor_millionths;
	pc->final_cltv = b11->min_final_cltv_expiry;
	pc->dest = type_to_string(cmd, struct node_id, &b11->receiver_id);
	pc->shadow_dest = tal_strdup(pc, pc->dest);
	pc->payment_hash = type_to_string(pc, struct sha256,
					  &b11->payment_hash);
	pc->stoptime = timeabs_add(time_now(), time_from_sec(*retryfor));
	pc->excludes = tal_arr(cmd, const char *, 0);
	pc->ps = add_pay_status(pc, b11str);
	if (b11->payment_secret)
		pc->payment_secret = tal_hexstr(pc, b11->payment_secret,
						sizeof(*b11->payment_secret));
	else
		pc->payment_secret = NULL;
	/* We try first without using routehint */
	pc->current_routehint = NULL;
	pc->routehints = filter_routehints(pc, b11->routes);
	pc->expensive_route = NULL;
#if DEVELOPER
	pc->use_shadow = *use_shadow;
#endif

	/* Get capacities of local channels (no parameters) */
	req = jsonrpc_request_start(cmd->plugin, cmd, "listpeers",
				    listpeers_done, forward_error, pc);
	return send_outreq(cmd->plugin, req);
}

/* FIXME: Add this to ccan/time? */
#define UTC_TIMELEN (sizeof("YYYY-mm-ddTHH:MM:SS.nnnZ"))
static void utc_timestring(const struct timeabs *time, char str[UTC_TIMELEN])
{
	char iso8601_msec_fmt[sizeof("YYYY-mm-ddTHH:MM:SS.%03dZ")];

	strftime(iso8601_msec_fmt, sizeof(iso8601_msec_fmt), "%FT%T.%%03dZ",
		 gmtime(&time->ts.tv_sec));
	snprintf(str, UTC_TIMELEN, iso8601_msec_fmt,
		 (int) time->ts.tv_nsec / 1000000);
}

static void add_attempt(struct json_stream *ret,
			const struct pay_status *ps,
			const struct pay_attempt *attempt)
{
	char timestr[UTC_TIMELEN];

	utc_timestring(&attempt->start, timestr);

	json_object_start(ret, NULL);
	json_add_string(ret, "strategy", attempt->why);
	json_add_string(ret, "start_time", timestr);
	json_add_u64(ret, "age_in_seconds",
		     time_to_sec(time_between(time_now(), attempt->start)));
	if (attempt->result || attempt->failure) {
		utc_timestring(&attempt->end, timestr);
		json_add_string(ret, "end_time", timestr);
		json_add_u64(ret, "duration_in_seconds",
			     time_to_sec(time_between(attempt->end,
						      attempt->start)));
	}
	if (tal_count(attempt->routehint)) {
		json_array_start(ret, "routehint");
		for (size_t i = 0; i < tal_count(attempt->routehint); i++) {
			json_object_start(ret, NULL);
			json_add_string(ret, "id",
					type_to_string(tmpctx, struct node_id,
						       &attempt->routehint[i].pubkey));
			json_add_string(ret, "channel",
					type_to_string(tmpctx,
						       struct short_channel_id,
						       &attempt->routehint[i].short_channel_id));
			json_add_u64(ret, "fee_base_msat",
				     attempt->routehint[i].fee_base_msat);
			json_add_u64(ret, "fee_proportional_millionths",
				     attempt->routehint[i].fee_proportional_millionths);
			json_add_u64(ret, "cltv_expiry_delta",
				     attempt->routehint[i].cltv_expiry_delta);
			json_object_end(ret);
		}
		json_array_end(ret);
	}
	if (tal_count(attempt->excludes)) {
		json_array_start(ret, "excluded_nodes_or_channels");
		for (size_t i = 0; i < tal_count(attempt->excludes); i++)
			json_add_string(ret, NULL, attempt->excludes[i]);
		json_array_end(ret);
	}

	if (attempt->route)
		json_add_jsonstr(ret, "route", attempt->route);

	if (attempt->failure)
		json_out_add_splice(ret->jout, "failure", attempt->failure);

	if (attempt->result)
		json_add_member(ret, "success", true, "%s", attempt->result);

	json_object_end(ret);
}

static void json_add_sendpay_result(struct json_stream *s, const struct payment_result *r)
{
	if (r->code != 0) {
		/* This is a failure */
		json_add_string(s, "message", r->message);
		json_add_u32(s, "code", r->code);

		json_object_start(s, "data");
		json_add_u32(s, "id", r->id);
		json_add_hex(s, "raw_message", r->raw_message, tal_bytelen(r->raw_message));
		json_add_num(s, "failcode", r->failcode);
		json_add_string(s, "failcodename", r->failcodename);

		if (r->erring_index)
			json_add_num(s, "erring_index", *r->erring_index);

		if (r->erring_node)
			json_add_node_id(s, "erring_node", r->erring_node);

		if (r->erring_channel)
			json_add_short_channel_id(s, "erring_channel",
						  r->erring_channel);

		if (r->erring_direction)
			json_add_num(s, "erring_direction",
				     *r->erring_direction);

		json_object_end(s);
	} else {
		/* This is a success */
		json_add_u32(s, "id", r->id);
		json_add_preimage(s, "payment_preimage", r->payment_preimage);
	}

}

static void paystatus_add_payment(struct json_stream *s, const struct payment *p)
{
	char timestr[UTC_TIMELEN];

	utc_timestring(&p->start_time, timestr);

	json_object_start(s, NULL);
	if (p->why != NULL)
		json_add_string(s, "strategy", p->why);
	json_add_string(s, "start_time", timestr);
	json_add_u64(s, "age_in_seconds",
		     time_to_sec(time_between(time_now(), p->start_time)));

	/* Any final state will have an end time. */
	if (p->step >= PAYMENT_STEP_SPLIT) {
		utc_timestring(&p->end_time, timestr);
		json_add_string(s, "end_time", timestr);
	}

	/* TODO Add routehint. */
	/* TODO Add route details */

	if (p->step < PAYMENT_STEP_SPLIT)
		json_add_string(s, "state", "pending");
	else
		json_add_string(s, "state", "completed");

	if (p->step == PAYMENT_STEP_SPLIT) {
		/* Don't add anything, this is neither a success nor a failure. */
	} else if (p->result != NULL) {
		if (p->step == PAYMENT_STEP_SUCCESS)
			json_object_start(s, "success");
		else
			json_object_start(s, "failure");
		json_add_sendpay_result(s, p->result);
		json_object_end(s);
	} else if (p->step >= PAYMENT_STEP_SPLIT) {
		json_object_start(s, "failure");
		json_add_num(s, "code", PAY_ROUTE_NOT_FOUND);
		json_add_string(s, "message", "Call to getroute: Could not find a route");
		json_object_end(s);
	}

	json_object_end(s);
	for (size_t i = 0; i < tal_count(p->children); i++)
		paystatus_add_payment(s, p->children[i]);
}

static struct command_result *json_paystatus(struct command *cmd,
					     const char *buf,
					     const jsmntok_t *params)
{
	struct pay_status *ps;
	const char *invstring;
	struct json_stream *ret;
	struct payment *p;

	if (!param(cmd, buf, params,
		   /* FIXME: rename to invstring */
		   p_opt("bolt11", param_string, &invstring),
		   NULL))
		return command_param_failed();

	ret = jsonrpc_stream_success(cmd);
	json_array_start(ret, "pay");

	/* FIXME: Index by bolt11 string! */
	/* TODO(cdecker) Remove once we migrated to `pay` with modifiers. */
	list_for_each(&pay_status, ps, list) {
		if (invstring && !streq(invstring, ps->invstring))
			continue;

		json_object_start(ret, NULL);
		json_add_invstring(ret, ps->invstring);
		json_add_u64(ret, "msatoshi",
			     ps->msat.millisatoshis); /* Raw: JSON */
		json_add_string(ret, "amount_msat",
				type_to_string(tmpctx, struct amount_msat,
					       &ps->msat));
		json_add_string(ret, "destination", ps->dest);
		if (ps->label)
			json_add_string(ret, "label", ps->label);
		if (ps->routehint_modifications)
			json_add_string(ret, "routehint_modifications",
					ps->routehint_modifications);
		if (ps->shadow && !streq(ps->shadow, ""))
			json_add_string(ret, "shadow", ps->shadow);
		if (ps->exclusions)
			json_add_string(ret, "local_exclusions", ps->exclusions);

		/* If it's in listpeers right now, this can be 0 */
		json_array_start(ret, "attempts");
		for (size_t i = 0; i < tal_count(ps->attempts); i++)
			add_attempt(ret, ps, &ps->attempts[i]);
		json_array_end(ret);
		json_object_end(ret);
	}

	list_for_each(&payments, p, list) {
		assert(p->parent == NULL);
		if (invstring && !streq(invstring, p->invstring))
			continue;

		json_object_start(ret, NULL);
		if (p->label != NULL)
			json_add_string(ret, "label", p->label);

		if (p->invstring)
			json_add_invstring(ret, p->invstring);
		json_add_amount_msat_only(ret, "amount_msat", p->amount);

		json_add_node_id(ret, "destination", p->destination);

		/* TODO(cdecker) Add label in once we track labels. */
		/* TODO(cdecker) Add routehint_modifications in once we track
		 * them. */
		/* TODO(cdecker) Add shadow route once we support it. */

		/* If it's in listpeers right now, this can be 0 */
		json_array_start(ret, "attempts");
		paystatus_add_payment(ret, p);
		json_array_end(ret);
		json_object_end(ret);
	}
	json_array_end(ret);

	return command_finished(cmd, ret);
}

static bool attempt_ongoing(const struct sha256 *payment_hash)
{
	struct pay_status *ps;
	struct payment *root;
	struct pay_attempt *attempt;
	struct payment_tree_result res;
	enum payment_step diff,
	    final_states = PAYMENT_STEP_FAILED | PAYMENT_STEP_SUCCESS;

	list_for_each(&pay_status, ps, list) {
		if (!sha256_eq(payment_hash, &ps->payment_hash))
			continue;
		attempt = &ps->attempts[tal_count(ps->attempts)-1];
		return attempt->result == NULL && attempt->failure == NULL;
	}

	list_for_each(&payments, root, list) {
		if (!sha256_eq(payment_hash, root->payment_hash))
			continue;
		res = payment_collect_result(root);
		diff = res.leafstates & ~final_states;
		return diff != 0;
	}
	return false;
}

/* A unique key for each payment attempt, even if the same invoice was
 * attempted multiple times. */
struct pay_sort_key {
	const struct sha256 *payment_hash;
	u64 groupid;
};

/* We consolidate multi-part payments into a single entry. */
struct pay_mpp {
	/* payment_hash from the invoice and lookup key */
	const struct sha256 *payment_hash;

	/* This is the bolt11/bolt12 string */
	const char *invstring;

	/* Accumulated states of all sendpay commands involved. */
	enum payment_result_state state;

	/* Optional label (of first one!) */
	const jsmntok_t *label;
	/* Optional preimage (iff status is successful) */
	const jsmntok_t *preimage;
	/* Only counts "complete" or "pending" payments. */
	size_t num_nonfailed_parts;
	/* Total amount sent ("complete" or "pending" only). */
	struct amount_msat amount_sent;

	/* Total amount received by the recipient ("complete" or "pending"
	 * only). Null if we have any part for which we didn't know the
	 * amount. */
	struct amount_msat *amount;

	/* Timestamp of the first part */
	u32 timestamp;

	/* The destination of the payment, if specified. */
	const jsmntok_t *destination;

	/* Which sendpay group is this? Necessary for invoices that have been
	 * attempted multiple times. */
	struct pay_sort_key sortkey;
};

static const struct pay_sort_key *pay_mpp_key(const struct pay_mpp *pm)
{
	return &pm->sortkey;
}

static size_t pay_mpp_hash(const struct pay_sort_key *key)
{
	struct siphash24_ctx ctx;
	siphash24_init(&ctx, siphash_seed());
	siphash24_update(&ctx, key->payment_hash, sizeof(struct sha256));
	siphash24_update(&ctx, &key->groupid, sizeof(u64));
	return siphash24_done(&ctx);
}

static bool pay_mpp_eq(const struct pay_mpp *pm, const struct pay_sort_key *key)
{
	return memcmp(pm->sortkey.payment_hash, key->payment_hash,
		      sizeof(struct sha256)) == 0 &&
		      pm->sortkey.groupid == key->groupid;
}

HTABLE_DEFINE_TYPE(struct pay_mpp, pay_mpp_key, pay_mpp_hash, pay_mpp_eq,
		   pay_map);

static void add_amount_sent(struct plugin *p,
			    const char *invstring,
			    struct pay_mpp *mpp,
			    const char *buf,
			    const jsmntok_t *t)
{
	struct amount_msat sent, recv;
	const jsmntok_t *msattok;


	json_to_msat(buf, json_get_member(buf, t, "amount_sent_msat"), &sent);
	if (!amount_msat_add(&mpp->amount_sent, mpp->amount_sent, sent))
		plugin_log(p, LOG_BROKEN,
			   "Cannot add amount_sent_msat for %s: %s + %s",
			   invstring,
			   type_to_string(tmpctx, struct amount_msat, &mpp->amount_sent),
			   type_to_string(tmpctx, struct amount_msat, &sent));

	msattok = json_get_member(buf, t, "amount_msat");

	/* If this is an unannotated partial payment we drop out estimate for
	 * all parts. */
	if (msattok == NULL) {
		mpp->amount = tal_free(mpp->amount);
		return;
	}

	/* If we had a part of this multi-part payment for which we don't know
	 * the amount, then this is NULL. No point in summing up if we don't
	 * have the exact value.*/
	if (mpp->amount == NULL)
		return;

	if (!json_to_msat(buf, msattok, &recv))
		plugin_err(p, "Cannot convert amount_sat %.*s",
			   json_tok_full_len(msattok),
			   json_tok_full(buf, msattok));

	if (!amount_msat_add(mpp->amount, *mpp->amount, recv))
		plugin_log(p, LOG_BROKEN,
			   "Cannot add amount_msat for %s: %s + %s",
			   invstring,
			   type_to_string(tmpctx, struct amount_msat, mpp->amount),
			   type_to_string(tmpctx, struct amount_msat, &sent));
}

static void add_new_entry(struct json_stream *ret,
			  const char *buf,
			  const struct pay_mpp *pm)
{
	json_object_start(ret, NULL);
	if (pm->invstring)
		json_add_invstring(ret, pm->invstring);

	if (pm->destination)
		json_add_tok(ret, "destination", pm->destination, buf);

	json_add_sha256(ret, "payment_hash", pm->payment_hash);

	if (pm->state & PAYMENT_COMPLETE)
		json_add_string(ret, "status", "complete");
	else if (pm->state & PAYMENT_PENDING || attempt_ongoing(pm->payment_hash))
		json_add_string(ret, "status", "pending");
	else
		json_add_string(ret, "status", "failed");

	json_add_u32(ret, "created_at", pm->timestamp);

	if (pm->label)
		json_add_tok(ret, "label", pm->label, buf);
	if (pm->preimage)
		json_add_tok(ret, "preimage", pm->preimage, buf);

	/* This is only tallied for pending and successful payments, not
	 * failures. */
	if (pm->amount != NULL && pm->num_nonfailed_parts > 0)
		json_add_string(ret, "amount_msat",
				fmt_amount_msat(tmpctx, *pm->amount));

	json_add_string(ret, "amount_sent_msat",
			fmt_amount_msat(tmpctx, pm->amount_sent));

	if (pm->num_nonfailed_parts > 1)
		json_add_u64(ret, "number_of_parts",
			     pm->num_nonfailed_parts);
	json_object_end(ret);
}

static struct command_result *listsendpays_done(struct command *cmd,
						const char *buf,
						const jsmntok_t *result,
						char *invstring)
{
	size_t i;
	const jsmntok_t *t, *arr;
	struct json_stream *ret;
	struct pay_map pay_map;
	struct pay_mpp *pm;
	struct pay_sort_key *order = tal_arr(tmpctx, struct pay_sort_key, 0);

	pay_map_init(&pay_map);

	arr = json_get_member(buf, result, "payments");
	if (!arr || arr->type != JSMN_ARRAY)
		return command_fail(cmd, LIGHTNINGD,
				    "Unexpected non-array result from listsendpays");

	json_for_each_arr(i, t, arr) {
		const jsmntok_t *status, *invstrtok, *hashtok, *createdtok, *grouptok;
		const char *invstr = invstring;
		struct sha256 payment_hash;
		u32 created_at;
		u64 groupid;
		struct pay_sort_key key;

		invstrtok = json_get_member(buf, t, "bolt11");
		if (!invstrtok)
			invstrtok = json_get_member(buf, t, "bolt12");
		hashtok = json_get_member(buf, t, "payment_hash");
		createdtok = json_get_member(buf, t, "created_at");
		assert(hashtok != NULL);
		assert(createdtok != NULL);

		grouptok = json_get_member(buf, t, "groupid");
		if (grouptok != NULL)
			json_to_u64(buf, grouptok, &groupid);
		else
			groupid = 0;

		json_to_sha256(buf, hashtok, &payment_hash);
		json_to_u32(buf, createdtok, &created_at);
		if (invstrtok)
			invstr = json_strdup(cmd, buf, invstrtok);

		key.payment_hash = &payment_hash;
		key.groupid = groupid;

		pm = pay_map_get(&pay_map, &key);
		if (!pm) {
			pm = tal(cmd, struct pay_mpp);
			pm->state = 0;
			pm->payment_hash = tal_dup(pm, struct sha256, &payment_hash);
			pm->invstring = tal_steal(pm, invstr);
			pm->destination = json_get_member(buf, t, "destination");
			pm->label = json_get_member(buf, t, "label");
			pm->preimage = NULL;
			pm->amount_sent = AMOUNT_MSAT(0);
			pm->amount = talz(pm, struct amount_msat);
			pm->num_nonfailed_parts = 0;
			pm->timestamp = created_at;
			pm->sortkey.payment_hash = pm->payment_hash;
			pm->sortkey.groupid = groupid;
			pay_map_add(&pay_map, pm);
			// First time we see the groupid we add it to the order
			// array, so we can retrieve them in the correct order.
			tal_arr_expand(&order, pm->sortkey);
		}

		status = json_get_member(buf, t, "status");
		if (json_tok_streq(buf, status, "complete")) {
			add_amount_sent(cmd->plugin, pm->invstring, pm, buf, t);
			pm->num_nonfailed_parts++;
			pm->preimage
				= json_get_member(buf, t, "payment_preimage");
			pm->state |= PAYMENT_COMPLETE;
		} else if (json_tok_streq(buf, status, "pending")) {
			add_amount_sent(cmd->plugin, pm->invstring, pm, buf, t);
			pm->num_nonfailed_parts++;
			pm->state |= PAYMENT_PENDING;
		} else {
			pm->state |= PAYMENT_FAILED;
		}
	}

	ret = jsonrpc_stream_success(cmd);
	json_array_start(ret, "pays");
	for (size_t i = 0; i < tal_count(order); i++) {
		pm = pay_map_get(&pay_map, &order[i]);
		assert(pm != NULL);
		add_new_entry(ret, buf, pm);
	}
	pay_map_clear(&pay_map);
	json_array_end(ret);
	return command_finished(cmd, ret);
}

static struct command_result *json_listpays(struct command *cmd,
					    const char *buf,
					    const jsmntok_t *params)
{
	const char *invstring, *status_str;
	struct sha256 *payment_hash;
	struct out_req *req;

	/* FIXME: would be nice to parse as a bolt11 so check worked in future */
	if (!param(cmd, buf, params,
		   /* FIXME: parameter should be invstring now */
		   p_opt("bolt11", param_string, &invstring),
		   p_opt("payment_hash", param_sha256, &payment_hash),
		   p_opt("status", param_string, &status_str),
		   NULL))
		return command_param_failed();

	req = jsonrpc_request_start(cmd->plugin, cmd, "listsendpays",
				    listsendpays_done, forward_error,
				    cast_const(char *, invstring));
	if (invstring)
		json_add_string(req->js, "bolt11", invstring);

	if (payment_hash)
		json_add_sha256(req->js, "payment_hash", payment_hash);

	if (status_str)
		json_add_string(req->js, "status", status_str);
	return send_outreq(cmd->plugin, req);
}

static const char *init(struct plugin *p,
			const char *buf UNUSED, const jsmntok_t *config UNUSED)
{
	rpc_scan(p, "getinfo", take(json_out_obj(NULL, NULL, NULL)),
		 "{id:%}", JSON_SCAN(json_to_node_id, &my_id));

	rpc_scan(p, "listconfigs",
		 take(json_out_obj(NULL, NULL, NULL)),
		 "{max-locktime-blocks:%,experimental-offers:%}",
		 JSON_SCAN(json_to_number, &maxdelay_default),
		 JSON_SCAN(json_to_bool, &exp_offers));

	return NULL;
}

static void on_payment_success(struct payment *payment)
{
	struct payment *p;
	struct payment_tree_result result = payment_collect_result(payment);
	struct json_stream *ret;
	assert(result.treestates & PAYMENT_STEP_SUCCESS);
	assert(result.leafstates & PAYMENT_STEP_SUCCESS);
	assert(result.preimage != NULL);

	/* Iterate through any pending payments we suspended and
	 * terminate them. */

	list_for_each(&payments, p, list) {
		/* The result for the active payment is returned in
		 * `payment_finished`. */
		if (payment == p)
			continue;
		if (!sha256_eq(payment->payment_hash, p->payment_hash))
			continue;
		if (p->cmd == NULL)
			continue;

		ret = jsonrpc_stream_success(p->cmd);
		json_add_node_id(ret, "destination", p->destination);
		json_add_sha256(ret, "payment_hash", p->payment_hash);
		json_add_timeabs(ret, "created_at", p->start_time);
		json_add_num(ret, "parts", result.attempts);

		json_add_amount_msat_compat(ret, p->amount, "msatoshi",
					    "amount_msat");
		json_add_amount_msat_compat(ret, result.sent, "msatoshi_sent",
					    "amount_sent_msat");

		if (result.leafstates != PAYMENT_STEP_SUCCESS)
			json_add_string(
				ret, "warning_partial_completion",
				"Some parts of the payment are not yet "
				"completed, but we have the confirmation "
				"from the recipient.");
		json_add_preimage(ret, "payment_preimage", result.preimage);

		json_add_string(ret, "status", "complete");
		if (command_finished(p->cmd, ret)) {/* Ignore result. */}
		p->cmd = NULL;
		list_del(&p->list);
	}
}

static void payment_add_attempt(struct json_stream *s, const char *fieldname, struct payment *p, bool recurse)
{
	bool finished = p->step >= PAYMENT_STEP_RETRY,
	     success = p->step == PAYMENT_STEP_SUCCESS;

	/* A fieldname is only reasonable if we're not recursing. Otherwise the
	 * fieldname would be reused for all attempts. */
	assert(!recurse || fieldname == NULL);

	json_object_start(s, fieldname);

	if (!finished)
		json_add_string(s, "status", "pending");
	else if (success)
		json_add_string(s, "status", "success");
	else
		json_add_string(s, "status", "failed");

	if (p->failreason != NULL)
		json_add_string(s, "failreason", p->failreason);

	json_add_u64(s, "partid", p->partid);
	json_add_amount_msat_only(s, "amount", p->amount);
	if (p->parent != NULL)
		json_add_u64(s, "parent_partid", p->parent->partid);

	json_object_end(s);
	for (size_t i=0; i<tal_count(p->children); i++) {
		payment_add_attempt(s, fieldname, p->children[i], recurse);
	}
}

static void payment_json_add_attempts(struct json_stream *s,
				      const char *fieldname, struct payment *p)
{
	assert(p == payment_root(p));
	json_array_start(s, fieldname);
	payment_add_attempt(s, NULL, p, true);
	json_array_end(s);
}

static void on_payment_failure(struct payment *payment)
{
	struct payment *p;
	struct payment_tree_result result = payment_collect_result(payment);
	list_for_each(&payments, p, list)
	{
		struct json_stream *ret;
		struct command *cmd;
		const char *msg;
		/* The result for the active payment is returned in
		 * `payment_finished`. */
		if (payment == p)
			continue;
		if (!sha256_eq(payment->payment_hash, p->payment_hash))
			continue;
		if (p->cmd == NULL)
			continue;

		cmd = p->cmd;

		if (p->aborterror != NULL) {
			/* We set an explicit toplevel error message,
			 * so let's report that. */
			ret = jsonrpc_stream_fail(cmd, PAY_STOPPED_RETRYING,
						  p->aborterror);
			payment_json_add_attempts(ret, "attempts", p);

			if (command_finished(cmd, ret)) {/* Ignore result. */}
		} else if (result.failure == NULL || result.failure->failcode < NODE) {
			/* This is failing because we have no more routes to try */
			msg = tal_fmt(cmd,
				      "Ran out of routes to try after "
				      "%d attempt%s: see `paystatus`",
				      result.attempts,
				      result.attempts == 1 ? "" : "s");
			ret = jsonrpc_stream_fail(cmd, PAY_STOPPED_RETRYING,
						  msg);
			payment_json_add_attempts(ret, "attempts", p);

			if (command_finished(cmd, ret)) {/* Ignore result. */}

		}  else {
			struct payment_result *failure = result.failure;
			assert(failure!= NULL);

			ret = jsonrpc_stream_fail(cmd, failure->code,
						  failure->message);

			json_add_u64(ret, "id", failure->id);

			json_add_u32(ret, "failcode", failure->failcode);
			json_add_string(ret, "failcodename",
					failure->failcodename);

			if (p->invstring)
				json_add_invstring(ret, p->invstring);

			json_add_hex_talarr(ret, "raw_message",
					    result.failure->raw_message);
			json_add_num(ret, "created_at", p->start_time.ts.tv_sec);
			json_add_node_id(ret, "destination", p->destination);
			json_add_sha256(ret, "payment_hash", p->payment_hash);
			// OK
			if (result.leafstates & PAYMENT_STEP_SUCCESS) {
				/* If one sub-payment succeeded then we have
				 * proof of payment, and the payment is a
				 * success. */
				json_add_string(ret, "status", "complete");

			} else if (result.leafstates & ~PAYMENT_STEP_FAILED) {
				/* If there are non-failed leafs we are still trying. */
				json_add_string(ret, "status", "pending");

			} else {
				json_add_string(ret, "status", "failed");
			}

			json_add_amount_msat_compat(ret, p->amount, "msatoshi",
						    "amount_msat");

			json_add_amount_msat_compat(ret, result.sent,
						    "msatoshi_sent",
						    "amount_sent_msat");

			if (failure != NULL) {
				if (failure->erring_index)
					json_add_num(ret, "erring_index",
						     *failure->erring_index);

				if (failure->erring_node)
					json_add_node_id(ret, "erring_node",
							 failure->erring_node);

				if (failure->erring_channel)
					json_add_short_channel_id(
					    ret, "erring_channel",
					    failure->erring_channel);

				if (failure->erring_direction)
					json_add_num(
					    ret, "erring_direction",
					    *failure->erring_direction);
			}

			if (command_finished(cmd, ret)) { /* Ignore result. */}
		}
		p->cmd = NULL;
		list_del(&p->list);
	}
}

/* We are interested in any prior attempts to pay this payment_hash /
 * invoice so we can set the `groupid` correctly and ensure we don't
 * already have a pending payment running. We also collect the summary
 * about an eventual previous complete payment so we can return that
 * as a no-op. */
static struct command_result *
payment_listsendpays_previous(struct command *cmd, const char *buf,
			      const jsmntok_t *result, struct payment *p)
{
	size_t i;
	const jsmntok_t *t, *arr, *err;
	/* What was the groupid of an eventual previous attempt? */
	u64 last_group = 0;
	/* Do we have pending sendpays for the previous attempt? */
	bool pending = false;
	/* Did a prior attempt succeed? */
	bool completed = false;

	/* Metadata for a complete payment, if one exists. */
	struct json_stream *ret;
	u32 parts = 0;
	struct preimage preimage;
	struct amount_msat sent, msat;
	struct node_id destination;
	u32 created_at;

	err = json_get_member(buf, result, "error");
	if (err)
		return command_fail(
			   cmd, LIGHTNINGD,
			   "Error retrieving previous pay attempts: %s",
			   json_strdup(tmpctx, buf, err));

			   arr = json_get_member(buf, result, "payments");
	if (!arr || arr->type != JSMN_ARRAY)
		return command_fail(
		    cmd, LIGHTNINGD,
		    "Unexpected non-array result from listsendpays");

	/* We iterate through all prior sendpays, looking for the
	 * latest group and remembering what its state is. */
	json_for_each_arr(i, t, arr)
	{
		u64 groupid;
		const jsmntok_t *status, *grouptok;
		struct amount_msat diff_sent, diff_msat;
		grouptok = json_get_member(buf, t, "groupid");
		json_to_u64(buf, grouptok, &groupid);

		/* New group, reset what we collected. */
		if (last_group != groupid) {
			completed = false;
			pending = false;
			last_group = groupid;

			parts = 1;
			json_scan(tmpctx, buf, t,
				  "{destination:%"
				  ",created_at:%"
				  ",amount_msat:%"
				  ",amount_sent_msat:%"
				  ",payment_preimage:%}",
				  JSON_SCAN(json_to_node_id, &destination),
				  JSON_SCAN(json_to_u32, &created_at),
				  JSON_SCAN(json_to_msat, &msat),
				  JSON_SCAN(json_to_msat, &sent),
				  JSON_SCAN(json_to_preimage, &preimage));
		} else {
			json_scan(tmpctx, buf, t,
				  "{amount_msat:%"
				  ",amount_sent_msat:%}",
				  JSON_SCAN(json_to_msat, &diff_msat),
				  JSON_SCAN(json_to_msat, &diff_sent));
			if (!amount_msat_add(&msat, msat, diff_msat) ||
			    !amount_msat_add(&sent, sent, diff_sent))
				plugin_err(p->plugin,
					   "msat overflow adding up parts");
			parts++;
		}

		status = json_get_member(buf, t, "status");
		completed |= json_tok_streq(buf, status, "complete");
		pending |= json_tok_streq(buf, status, "pending");
	}

	if (completed) {
		ret = jsonrpc_stream_success(cmd);
		json_add_preimage(ret, "payment_preimage", &preimage);
		json_add_string(ret, "status", "complete");
		json_add_amount_msat_compat(ret, msat, "msatoshi",
					    "amount_msat");
		json_add_amount_msat_compat(ret, sent, "msatoshi_sent",
					    "amount_sent_msat");
		json_add_node_id(ret, "destination", p->destination);
		json_add_sha256(ret, "payment_hash", p->payment_hash);
		json_add_u32(ret, "created_at", created_at);
		json_add_num(ret, "parts", parts);
		return command_finished(cmd, ret);
	} else if (pending) {
		/* We suspend this call and wait for the
		 * `on_payment_success` or `on_payment_failure`
		 * handler of the currently running payment to notify
		 * us about its completion. */
		return command_still_pending(cmd);
	}
	p->groupid = last_group + 1;
	p->on_payment_success = on_payment_success;
	p->on_payment_failure = on_payment_failure;
	payment_start(p);
	return command_still_pending(cmd);
}

struct payment_modifier *paymod_mods[] = {
	/* NOTE: The order in which these four paymods are executed is
	 * significant!
	 * local_channel_hints *must* execute first before route_exclusions
	 * which *must* execute before directpay.
	 * exemptfee *must* also execute before directpay.
	 */
	&local_channel_hints_pay_mod,
	&route_exclusions_pay_mod,
	&exemptfee_pay_mod,
	&directpay_pay_mod,
	&shadowroute_pay_mod,
	/* NOTE: The order in which these three paymods are executed is
	 * significant!
	 * routehints *must* execute first before payee_incoming_limit
	 * which *must* execute bfore presplit.
	 *
	 * FIXME: Giving an ordered list of paymods to the paymod
	 * system is the wrong interface, given that the order in
	 * which paymods execute is significant.
	 * (This is typical of Entity-Component-System pattern.)
	 * What should be done is that libplugin-pay should have a
	 * canonical list of paymods in the order they execute
	 * correctly, and whether they are default-enabled/default-disabled,
	 * and then clients like `pay` and `keysend` will disable/enable
	 * paymods that do not help them, instead of the current interface
	 * where clients provide an *ordered* list of paymods they want to
	 * use.
	 */
	&routehints_pay_mod,
	&payee_incoming_limit_pay_mod,
	&presplit_pay_mod,
	&waitblockheight_pay_mod,
	&retry_pay_mod,
	&adaptive_splitter_pay_mod,
	NULL,
};

static struct command_result *json_paymod(struct command *cmd,
					  const char *buf,
					  const jsmntok_t *params)
{
	struct payment *p;
	const char *b11str;
	struct bolt11 *b11;
	char *b11_fail, *b12_fail;
	u64 *maxfee_pct_millionths;
	u32 *maxdelay;
	struct amount_msat *exemptfee, *msat;
	const char *label;
	unsigned int *retryfor;
	u64 *riskfactor_millionths;
	struct shadow_route_data *shadow_route;
	struct amount_msat *invmsat;
	u64 invexpiry;
	struct sha256 *local_offer_id;
	const struct tlv_invoice *b12;
	struct out_req *req;
	struct route_exclusion **exclusions;
#if DEVELOPER
	bool *use_shadow;
#endif

	/* If any of the modifiers need to add params to the JSON-RPC call we
	 * would add them to the `param()` call below, and have them be
	 * initialized directly that way. */
	if (!param(cmd, buf, params,
		   /* FIXME: parameter should be invstring now */
		   p_req("bolt11", param_string, &b11str),
		   p_opt("msatoshi", param_msat, &msat),
		   p_opt("label", param_string, &label),
		   p_opt_def("riskfactor", param_millionths,
			     &riskfactor_millionths, 10000000),
		   p_opt_def("maxfeepercent", param_millionths,
			     &maxfee_pct_millionths, 500000),
		   p_opt_def("retry_for", param_number, &retryfor, 60),
		   p_opt_def("maxdelay", param_number, &maxdelay,
			     maxdelay_default),
		   p_opt_def("exemptfee", param_msat, &exemptfee, AMOUNT_MSAT(5000)),
		   p_opt("localofferid", param_sha256, &local_offer_id),
		   p_opt("exclude", param_route_exclusion_array, &exclusions),
#if DEVELOPER
		   p_opt_def("use_shadow", param_bool, &use_shadow, true),
#endif
		      NULL))
		return command_param_failed();

	p = payment_new(cmd, cmd, NULL /* No parent */, paymod_mods);
	p->invstring = tal_steal(p, b11str);

	if (!bolt12_has_prefix(b11str)) {
		b11 =
		    bolt11_decode(p, b11str, plugin_feature_set(cmd->plugin),
				  NULL, chainparams, &b11_fail);
		if (b11 == NULL)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "Invalid bolt11: %s", b11_fail);

		invmsat = b11->msat;
		invexpiry = b11->timestamp + b11->expiry;

		p->destination = tal_dup(p, struct node_id, &b11->receiver_id);
		p->destination_has_tlv =
		    feature_offered(b11->features, OPT_VAR_ONION);
		p->payment_hash = tal_dup(p, struct sha256, &b11->payment_hash);
		p->payment_secret =
		    b11->payment_secret
			? tal_dup(p, struct secret, b11->payment_secret)
			: NULL;
		p->routes = tal_steal(p, b11->routes);
		p->min_final_cltv_expiry = b11->min_final_cltv_expiry;
		p->features = tal_steal(p, b11->features);
		/* Sanity check */
		if (feature_offered(b11->features, OPT_VAR_ONION) &&
		    !b11->payment_secret)
			return command_fail(
			    cmd, JSONRPC2_INVALID_PARAMS,
			    "Invalid bolt11:"
			    " sets feature var_onion with no secret");
	} else {
		b12 = invoice_decode(p, b11str, strlen(b11str),
				     plugin_feature_set(cmd->plugin),
				     chainparams, &b12_fail);
		if (b12 == NULL)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "Invalid bolt12: %s", b12_fail);
		if (!exp_offers)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "experimental-offers disabled");

		p->features = tal_steal(p, b12->features);

		if (!b12->node_id)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "invoice missing node_id");
		if (!b12->payment_hash)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "invoice missing payment_hash");
		if (!b12->created_at)
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "invoice missing created_at");
		if (b12->amount) {
			invmsat = tal(cmd, struct amount_msat);
			*invmsat = amount_msat(*b12->amount);
		} else
			invmsat = NULL;

		/* FIXME: gossmap should store as point32 */
		p->destination = tal(p, struct node_id);
		gossmap_guess_node_id(get_gossmap(cmd->plugin), b12->node_id,
				      p->destination);
		p->destination_has_tlv = true;
		p->payment_hash = tal_dup(p, struct sha256, b12->payment_hash);
		if (b12->recurrence_counter && !label)
			return command_fail(
			    cmd, JSONRPC2_INVALID_PARAMS,
			    "recurring invoice requires a label");
		/* FIXME payment_secret should be signature! */
		{
			struct sha256 merkle;

			p->payment_secret = tal(p, struct secret);
			merkle_tlv(b12->fields, &merkle);
			memcpy(p->payment_secret, &merkle, sizeof(merkle));
			BUILD_ASSERT(sizeof(*p->payment_secret) ==
				     sizeof(merkle));
		}
		p->routes = NULL;
		/* FIXME: paths! */
		if (b12->cltv)
			p->min_final_cltv_expiry = *b12->cltv;
		else
			p->min_final_cltv_expiry = 18;
		/* BOLT-offers #12:
		 * - if `relative_expiry` is present:
		 *   - MUST reject the invoice if the current time since
		 *     1970-01-01 UTC is greater than `created_at` plus
		 *     `seconds_from_creation`.
		 * - otherwise:
		 *   - MUST reject the invoice if the current time since
		 *     1970-01-01 UTC is greater than `created_at` plus
		 * 7200.
		 */
		if (b12->relative_expiry)
			invexpiry = *b12->created_at + *b12->relative_expiry;
		else
			invexpiry = *b12->created_at + BOLT12_DEFAULT_REL_EXPIRY;
		p->local_offer_id = tal_steal(p, local_offer_id);
	}

	if (time_now().ts.tv_sec > invexpiry)
		return command_fail(cmd, PAY_INVOICE_EXPIRED, "Invoice expired");

	if (invmsat) {
		if (msat) {
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "msatoshi parameter unnecessary");
		}
		p->amount = *invmsat;

	} else {
		if (!msat) {
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
					    "msatoshi parameter required");
		}
		p->amount = *msat;
	}

	if (node_id_eq(&my_id, p->destination))
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "This payment is destined for ourselves. "
				    "Self-payments are not supported");

	p->local_id = &my_id;
	p->json_buffer = tal_steal(p, buf);
	p->json_toks = params;
	p->why = "Initial attempt";
	p->constraints.cltv_budget = *maxdelay;
	p->deadline = timeabs_add(time_now(), time_from_sec(*retryfor));
	p->getroute->riskfactorppm = *riskfactor_millionths;

	if (!amount_msat_fee(&p->constraints.fee_budget, p->amount, 0,
			     *maxfee_pct_millionths / 100)) {
		tal_free(p);
		return command_fail(
		    cmd, JSONRPC2_INVALID_PARAMS,
		    "Overflow when computing fee budget, fee rate too high.");
	}
	p->constraints.cltv_budget = *maxdelay;

	payment_mod_exemptfee_get_data(p)->amount = *exemptfee;
	shadow_route = payment_mod_shadowroute_get_data(p);
	payment_mod_presplit_get_data(p)->disable = disablempp;
	payment_mod_adaptive_splitter_get_data(p)->disable = disablempp;
	payment_mod_route_exclusions_get_data(p)->exclusions = exclusions;

	/* This is an MPP enabled pay command, disable amount fuzzing. */
	shadow_route->fuzz_amount = false;
#if DEVELOPER
	shadow_route->use_shadow = *use_shadow;
#endif
	p->label = tal_steal(p, label);
	list_add_tail(&payments, &p->list);
	/* We're keeping this around now */
	tal_steal(cmd->plugin, p);

	req = jsonrpc_request_start(cmd->plugin, cmd, "listsendpays",
				    payment_listsendpays_previous,
				    payment_listsendpays_previous, p);

	json_add_sha256(req->js, "payment_hash", p->payment_hash);
	return send_outreq(cmd->plugin, req);
}

static const struct plugin_command commands[] = {
#ifdef COMPAT_V090
	{
		"legacypay",
		"payment",
		"Send payment specified by {bolt11} with {amount}",
		"Try to send a payment, retrying {retry_for} seconds before giving up",
		json_pay
	},
#endif
	{
		"paystatus",
		"payment",
		"Detail status of attempts to pay {bolt11}, or all",
		"Covers both old payments and current ones.",
		json_paystatus
	}, {
		"listpays",
		"payment",
		"List result of payment {bolt11} or {payment_hash}, or all",
		"Covers old payments (failed and succeeded) and current ones.",
		json_listpays
	},
	{
		"pay",
		"payment",
		"Send payment specified by {bolt11}",
		"Attempt to pay the {bolt11} invoice.",
		json_paymod
	},
};

static const char *notification_topics[] = {
	"pay_success",
	"pay_failure",
};

int main(int argc, char *argv[])
{
	setup_locale();
	plugin_main(argv, init, PLUGIN_RESTARTABLE, true, NULL, commands,
		    ARRAY_SIZE(commands), NULL, 0, NULL, 0,
		    notification_topics, ARRAY_SIZE(notification_topics),
		    plugin_option("disable-mpp", "flag",
				  "Disable multi-part payments.",
				  flag_option, &disablempp),
		    NULL);
}
