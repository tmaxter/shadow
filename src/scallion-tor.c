/**
 * The Shadow Simulator
 *
 * Copyright (c) 2010-2011 Rob Jansen <jansen@cs.umn.edu>
 *
 * This file is part of Shadow.
 *
 * Shadow is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Shadow is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Shadow.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "scallion.h"
#include <openssl/rand.h>

// this should only appear if Tor > 0.2.3.5-alpha
// handled in setup.py and CMakelists.txt
//#define DOREFILL

/* replacement for torflow in Tor. for now just grab the bandwidth we configured
 * in the DSIM and use that as the measured bandwidth value. since our configured
 * bandwidth doesnt change over time, this could just be run once (by setting the
 * time far in the future so the file is not seen as outdated). but we need to
 * run it after all routers are loaded, so its best to re-run periodically.
 *
 * eventually we will want an option to run something similar to the actual
 * torflow scripts that download files over Tor and computes bandwidth values.
 * in that case it needs to run more often to keep monitoring the actual state
 * of the network.
 *
 * torflow writes a few things to the v3bwfile. all Tor currently uses is:
 *
 * 0123456789
 * node_id=$0123456789ABCDEF0123456789ABCDEF01234567 bw=12345
 * ...
 *
 * where 0123456789 is the time, 0123456789ABCDEF0123456789ABCDEF01234567 is
 * the relay's fingerprint, and 12345 is the measured bandwidth in ?.
 */
void scalliontor_init_v3bw(ScallionTor* stor) {
	/* open the bw file, clearing it if it exists */
	FILE *v3bw = fopen(stor->v3bw_name, "w");
	if(v3bw == NULL) {
		stor->shadowlibFuncs->log(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
				"v3bandwidth file not updated: can not open file '%s'\n", stor->v3bw_name);
		return;
	}

	time_t maxtime = -1;

	/* print time part on first line */
	if(fprintf(v3bw, "%lu\n", maxtime) < 0) {
		/* uhhhh... */
		stor->shadowlibFuncs->log(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
		"v3bandwidth file not updated: can write time '%u' to file '%s'\n", maxtime, stor->v3bw_name);
		return;
	}

	routerlist_t *rlist = router_get_routerlist();
	routerinfo_t *rinfo;

	/* print an entry for each router */
	for (int i=0; i < smartlist_len(rlist->routers); i++) {
		rinfo = smartlist_get(rlist->routers, i);

		/* get the fingerprint from its digest */
		char node_id[HEX_DIGEST_LEN+1];
		base16_encode(node_id, HEX_DIGEST_LEN+1, rinfo->cache_info.identity_digest, DIGEST_LEN);

		/* the network address */
		in_addr_t netaddr = htonl(rinfo->addr);

		/* ask shadow for this node's configured bandwidth */
		guint bwdown = 0, bwup = 0;
		stor->shadowlibFuncs->getBandwidth(netaddr, &bwdown, &bwup);

		guint bw = MIN(bwup, bwdown);

		if(fprintf(v3bw, "node_id=$%s bw=%u\n", node_id, bw) < 0) {
			/* uhhhh... */
			stor->shadowlibFuncs->log(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"v3bandwidth file not updated: can write line 'node_id=$%s bw=%u\n' to file '%s'\n", node_id, bw, stor->v3bw_name);
			return;
		}
	}

	fclose(v3bw);

	/* reschedule */
	stor->shadowlibFuncs->createCallback((ShadowPluginCallbackFunc)scalliontor_init_v3bw, (gpointer)stor, VTORFLOW_SCHED_PERIOD);
}

void scalliontor_free(ScallionTor* stor) {
	tor_cleanup();
	g_free(stor);
}

static void _scalliontor_secondCallback(ScallionTor* stor) {
	scalliontor_notify(stor);

	/* call Tor's second elapsed function */
	second_elapsed_callback(NULL, NULL);

	/* make sure we handle any event creations that happened in Tor */
	scalliontor_notify(stor);

	/* schedule the next callback */
	if(stor) {
		stor->shadowlibFuncs->createCallback((ShadowPluginCallbackFunc)_scalliontor_secondCallback,
				stor, 1000);
	}
}

#ifdef DOREFILL
static void _scalliontor_refillCallback(ScallionTor* stor) {
	scalliontor_notify(stor);

	/* call Tor's refill function */
	refill_callback(NULL, NULL);

	/* make sure we handle any event creations that happened in Tor */
	scalliontor_notify(stor);

	/* schedule the next callback */
	if(stor) {
		stor->shadowlibFuncs->createCallback((ShadowPluginCallbackFunc)_scalliontor_refillCallback,
				stor, stor->refillmsecs);
	}
}
#endif

gint scalliontor_start(ScallionTor* stor, gint argc, gchar *argv[]) {
	time_t now = time(NULL);

	update_approx_time(now);
	tor_threads_init();
	init_logging();

	if (tor_init(argc, argv) < 0) {
		return -1;
	}

	  /* load the private keys, if we're supposed to have them, and set up the
	   * TLS context. */
	gpointer idkey;
#ifdef DOREFILL // FIXME this doesnt change in 0.2.3.5-alpha like DOREFILL is meant to (not sure when it changed)
	idkey = client_identitykey;
#else
	idkey = identitykey;
#endif
    if (idkey == NULL) {
	  if (init_keys() < 0) {
	    log_err(LD_BUG,"Error initializing keys; exiting");
	    return -1;
	  }
    }

	/* Set up the packed_cell_t memory pool. */
	init_cell_pool();

	/* Set up our buckets */
	connection_bucket_init();
	stats_prev_global_read_bucket = global_read_bucket;
	stats_prev_global_write_bucket = global_write_bucket;

	/* initialize the bootstrap status events to know we're starting up */
	control_event_bootstrap(BOOTSTRAP_STATUS_STARTING, 0);

	if (trusted_dirs_reload_certs()) {
		log_warn(LD_DIR,
			 "Couldn't load all cached v3 certificates. Starting anyway.");
	}
	if (router_reload_v2_networkstatus()) {
		return -1;
	}
	if (router_reload_consensus_networkstatus()) {
		return -1;
	}

	/* load the routers file, or assign the defaults. */
	if (router_reload_router_list()) {
		return -1;
	}

	/* load the networkstatuses. (This launches a download for new routers as
	* appropriate.)
	*/
	directory_info_has_arrived(now, 1);

	/* !note that scallion intercepts the cpuworker functionality (rob) */
	if (server_mode(get_options())) {
		/* launch cpuworkers. Need to do this *after* we've read the onion key. */
		cpu_init();
	}

	/* set up once-a-second callback. */
	if (! second_timer) {
//		struct timeval one_second;
//		one_second.tv_sec = 1;
//		one_second.tv_usec = 0;
//
//		second_timer = periodic_timer_new(tor_libevent_get_base(),
//										  &one_second,
//										  second_elapsed_callback,
//										  NULL);
//		tor_assert(second_timer);

		_scalliontor_secondCallback(stor);
	}


#ifdef DOREFILL
#ifndef USE_BUFFEREVENTS
  if (!refill_timer) {
    int msecs = get_options()->TokenBucketRefillInterval;
//    struct timeval refill_interval;
//
//    refill_interval.tv_sec =  msecs/1000;
//    refill_interval.tv_usec = (msecs%1000)*1000;
//
//    refill_timer = periodic_timer_new(tor_libevent_get_base(),
//                                      &refill_interval,
//                                      refill_callback,
//                                      NULL);
//    tor_assert(refill_timer);
    stor->refillmsecs = msecs;
	_scalliontor_refillCallback(stor);
  }
#endif
#endif

    /* run the startup events */
    scalliontor_notify(stor);

	return 0;
}

ScallionTor* scalliontor_new(ShadowlibFunctionTable* shadowlibFuncs, char* hostname, enum vtor_nodetype type,
		char* bandwidth, char* torrc_path, char* datadir_path, char* geoip_path) {
	ScallionTor* stor = g_new0(ScallionTor, 1);
	stor->shadowlibFuncs = shadowlibFuncs;

	stor->type = type;
	stor->bandwidth = atoi(bandwidth);

	/* we use 14 args to tor by 'default' */
	int num_args = 21;
	if(stor->type == VTOR_DIRAUTH || stor->type == VTOR_RELAY) {
		num_args += 2;
	}

	char bwconf[128];
	snprintf(bwconf, 128, "%s KB", bandwidth);

	int cap = stor->bandwidth + 5120;
	int burst = stor->bandwidth * 2;
	if(burst > cap) {
		burst = cap;
	}
	char burstconf[128];
	snprintf(burstconf, 128, "%i KB", burst);

	/* default args */
	char *config[num_args];
	config[0] = "tor";
	config[1] = "--Address";
	config[2] = hostname;
	config[3] = "-f";
	config[4] = torrc_path;
	config[5] = "--DataDirectory";
	config[6] = datadir_path;
	config[7] = "--GeoIPFile";
	config[8] = geoip_path;
	config[9] = "--BandwidthRate";
	config[10] = bwconf;
	config[11] = "--BandwidthBurst";
	config[12] = bwconf;
	config[13] = "--MaxAdvertisedBandwidth";
	config[14] = bwconf;
	config[15] = "--RelayBandwidthRate";
	config[16] = bwconf;
	config[17] = "--RelayBandwidthBurst";
	config[18] = bwconf;

	gchar* nickname = g_strdup(hostname);
	while(1) {
		gchar* dot = g_strstr_len((const gchar*)nickname, -1, ".");
		if(dot != NULL) {
			*dot = 'x';
		} else {
			break;
		}
	}

	config[19] = "--Nickname";
	config[20] = nickname;

	/* additional args */
	if(stor->type == VTOR_DIRAUTH) {
		if(snprintf(stor->v3bw_name, 255, "%s/dirauth.v3bw", datadir_path) >= 255) {
			stor->shadowlibFuncs->log(G_LOG_LEVEL_MESSAGE, __FUNCTION__,
					"data directory path is too long and was truncated to '%s'\n", stor->v3bw_name);
		}
		config[21] = "--V3BandwidthsFile";
		config[22] = stor->v3bw_name;
	} else if(stor->type == VTOR_RELAY) {
		config[21] = "--ExitPolicy";
		config[22] = "reject *:*";
	}

	/* Shadow intercepts RAND_get_rand_method. get pointers to its funcs. */
	const RAND_METHOD* shadowRandomMethod = RAND_get_rand_method();
	/* we need to make sure OpenSSL is using Shadow for randomness. */
	RAND_set_rand_method(shadowRandomMethod);

	scallion.stor = stor;
	scalliontor_start(stor, num_args, config);

	if(stor->type == VTOR_DIRAUTH) {
		/* run torflow now, it will schedule itself as needed */
		scalliontor_init_v3bw(stor);
	}

	g_free(nickname);

	return stor;
}

void scalliontor_notify(ScallionTor* stor) {
	update_approx_time(time(NULL));

	/* tell libevent to check epoll and activate the ready sockets without blocking */
	event_base_loop(tor_libevent_get_base(), EVLOOP_NONBLOCK);
}

/*
 * normally tor calls event_base_loopexit so control returns from the libevent
 * event loop back to the tor main loop. tor then activates "linked" socket
 * connections before returning back to the libevent event loop.
 *
 * we hijack and use the libevent loop in nonblock mode, so when tor calls
 * the loopexit, we basically just need to do the linked connection activation.
 * that is extracted to scalliontor_loopexitCallback, which we need to execute
 * as a callback so we don't invoke event_base_loop while it is currently being
 * executed. */
static void scalliontor_loopexitCallback(ScallionTor* stor) {
	update_approx_time(time(NULL));

	scalliontor_notify(stor);

	while(1) {
		/* All active linked conns should get their read events activated. */
		SMARTLIST_FOREACH(active_linked_connection_lst, connection_t *, conn,
				event_active(conn->read_event, EV_READ, 1));

		/* if linked conns are still active, enter libevent loop using EVLOOP_ONCE */
		called_loop_once = smartlist_len(active_linked_connection_lst) ? 1 : 0;
		if(called_loop_once) {
			event_base_loop(tor_libevent_get_base(), EVLOOP_ONCE|EVLOOP_NONBLOCK);
		} else {
			/* linked conns are done */
			break;
		}
	}

	/* make sure we handle any new events caused by the linked conns */
	scalliontor_notify(stor);
}
void scalliontor_loopexit(ScallionTor* stor) {
	stor->shadowlibFuncs->createCallback((ShadowPluginCallbackFunc)scalliontor_loopexitCallback, (gpointer)stor, 1);
}

void scalliontor_readCPUWorkerCallback(int sockd, short ev_types, void * arg) {
	/* taken from cpuworker_main.
	 *
	 * these are blocking calls in Tor. we need to cope, so the approach we
	 * take is that if the first read would block, its still ok. after
	 * that, we fail if the rest of what we expect isnt there.
	 *
	 * FIXME make this completely nonblocking with a state machine.
	 */
	vtor_cpuworker_tp cpuw = arg;

	if(cpuw != NULL) {
		ssize_t r = 0;

		r = recv(cpuw->fd, &(cpuw->question_type), 1, 0);

		if(r < 0) {
			if(errno == EAGAIN) {
				/* dont block! and dont fail! */
				goto ret;
			} else {
				/* true error from shadow network layer */
				log_info(LD_OR,
						 "CPU worker exiting because of error on connection to Tor "
						 "process.");
				log_info(LD_OR,"(Error on %d was %s)",
						cpuw->fd, tor_socket_strerror(tor_socket_errno(cpuw->fd)));
				goto end;
			}
		} else if (r == 0) {
			log_info(LD_OR,
					 "CPU worker exiting because Tor process closed connection "
					 "(either rotated keys or died).");
			goto end;
		}

		/* we got our initial question */

		tor_assert(cpuw->question_type == CPUWORKER_TASK_ONION);

		r = read_all(cpuw->fd, cpuw->tag, TAG_LEN, 1);

		if (r != TAG_LEN) {
		  log_err(LD_BUG,"read tag failed. Exiting.");
		  goto end;
		}

		r = read_all(cpuw->fd, cpuw->question, ONIONSKIN_CHALLENGE_LEN, 1);

		if (r != ONIONSKIN_CHALLENGE_LEN) {
		  log_err(LD_BUG,"read question failed. Exiting.");
		  goto end;
		}

		if (cpuw->question_type == CPUWORKER_TASK_ONION) {
			r = onion_skin_server_handshake(cpuw->question, cpuw->onion_key, cpuw->last_onion_key,
					  cpuw->reply_to_proxy, cpuw->keys, CPATH_KEY_MATERIAL_LEN);

			if (r < 0) {
				/* failure */
				log_debug(LD_OR,"onion_skin_server_handshake failed.");
				*(cpuw->buf) = 0; /* indicate failure in first byte */
				memcpy(cpuw->buf+1,cpuw->tag,TAG_LEN);
				/* send all zeros as answer */
				memset(cpuw->buf+1+TAG_LEN, 0, LEN_ONION_RESPONSE-(1+TAG_LEN));
			} else {
				/* success */
				log_debug(LD_OR,"onion_skin_server_handshake succeeded.");
				cpuw->buf[0] = 1; /* 1 means success */
				memcpy(cpuw->buf+1,cpuw->tag,TAG_LEN);
				memcpy(cpuw->buf+1+TAG_LEN,cpuw->reply_to_proxy,ONIONSKIN_REPLY_LEN);
				memcpy(cpuw->buf+1+TAG_LEN+ONIONSKIN_REPLY_LEN,cpuw->keys,CPATH_KEY_MATERIAL_LEN);
			}

			r = write_all(cpuw->fd, cpuw->buf, LEN_ONION_RESPONSE, 1);

			if (r != LEN_ONION_RESPONSE) {
				log_err(LD_BUG,"writing response buf failed. Exiting.");
				goto end;
			}

			log_debug(LD_OR,"finished writing response.");
		}
	}
ret:
	return;
end:
	if(cpuw != NULL) {
		if (cpuw->onion_key)
			crypto_free_pk_env(cpuw->onion_key);
		if (cpuw->last_onion_key)
			crypto_free_pk_env(cpuw->last_onion_key);
		tor_close_socket(cpuw->fd);
		event_del(&(cpuw->read_event));
		free(cpuw);
	}
}

void scalliontor_newCPUWorker(ScallionTor* stor, int fd) {
	g_assert(stor);
	if(stor->cpuw) {
		g_free(stor->cpuw);
	}

	vtor_cpuworker_tp cpuw = malloc(sizeof(vtor_cpuworker_t));

	cpuw->fd = fd;
	cpuw->onion_key = NULL;
	cpuw->last_onion_key = NULL;

	dup_onion_keys(&(cpuw->onion_key), &(cpuw->last_onion_key));

	/* setup event so we will get a callback */
	event_assign(&(cpuw->read_event), tor_libevent_get_base(), cpuw->fd, EV_READ|EV_PERSIST, scalliontor_readCPUWorkerCallback, cpuw);
	event_add(&(cpuw->read_event), NULL);
}

static ScallionTor* scalliontor_getPointer() {
	return scallion.stor;
}

/*
 * Tor function interceptions
 */

int intercept_event_base_loopexit(struct event_base * base, const struct timeval * t) {
	ScallionTor* stor = scalliontor_getPointer();
	g_assert(stor);

	scalliontor_loopexit(stor);
	return 0;
}

int intercept_tor_open_socket(int domain, int type, int protocol)
{
  int s = socket(domain, type | SOCK_NONBLOCK, protocol);
  if (s >= 0) {
    socket_accounting_lock();
    ++n_sockets_open;
//    mark_socket_open(s);
    socket_accounting_unlock();
  }
  return s;
}

void intercept_tor_gettimeofday(struct timeval *timeval) {
	struct timespec tp;
	clock_gettime(CLOCK_REALTIME, &tp);
	timeval->tv_sec = tp.tv_sec;
	timeval->tv_usec = tp.tv_nsec/1000;
}

void intercept_logv(int severity, uint32_t domain, const char *funcname,
     const char *format, va_list ap) {
	char* sev_str = NULL;
	const size_t buflen = 10024;
	char buf[buflen];
	size_t current_position = 0;

	/* Call assert, not tor_assert, since tor_assert calls log on failure. */
	assert(format);

	GLogLevelFlags level;

	switch (severity) {
		case LOG_DEBUG:
			sev_str = "tor-debug";
			level = G_LOG_LEVEL_DEBUG;
		break;

		case LOG_INFO:
			sev_str = "tor-info";
			level = G_LOG_LEVEL_INFO;
		break;

		case LOG_NOTICE:
			sev_str = "tor-notice";
			level = G_LOG_LEVEL_MESSAGE;
		break;

		case LOG_WARN:
			sev_str = "tor-warn";
			level = G_LOG_LEVEL_WARNING;
		break;

		case LOG_ERR:
			sev_str = "tor-err";
			level = G_LOG_LEVEL_ERROR;
		break;

		default:
			sev_str = "tor-UNKNOWN";
			level = G_LOG_LEVEL_DEBUG;
		break;
	}

	snprintf(&buf[current_position], strlen(sev_str)+4, "[%s] ", sev_str);
	current_position += strlen(sev_str)+3;

	if (domain == LD_BUG) {
		snprintf(&buf[current_position], 6, "BUG: ");
		current_position += 5;
	}

	if(funcname != NULL) {
		snprintf(&buf[current_position], strlen(funcname)+4, "%s() ", funcname);
		current_position += strlen(funcname)+3;
	}

	size_t size = buflen-current_position-2;
	int res = vsnprintf(&buf[current_position], size, format, ap);

	if(res >= size) {
		/* truncated */
		current_position = buflen - 3;
	} else {
		current_position += res;
	}

	buf[current_position] = '\0';
	current_position++;
	scallion.shadowlibFuncs->log(level, __FUNCTION__, buf);
}

int intercept_spawn_func(void (*func)(void *), void *data)
{
	ScallionTor* stor = scalliontor_getPointer();
	g_assert(stor);

	/* this takes the place of forking a cpuworker and running cpuworker_main.
	 * func points to cpuworker_main, but we'll implement a version that
	 * works in shadow */
	int *fdarray = data;
	int fd = fdarray[1]; /* this side is ours */

	scalliontor_newCPUWorker(stor, fd);

	/* now we should be ready to receive events in vtor_cpuworker_readable */
	return 0;
}

int intercept_rep_hist_bandwidth_assess() {
	ScallionTor* stor = scalliontor_getPointer();
	g_assert(stor);

	/* need to convert to bytes. tor will divide the value we return by 1000 and put it in the descriptor. */
	int bw = INT_MAX;
	if((stor->bandwidth * 1000) < bw) {
		bw = (stor->bandwidth * 1000);
	}
	return bw;
}
