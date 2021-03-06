#define _POSIX_C_SOURCE 200809L
#define _BSD_SOURCE 1
#define _GNU_SOURCE 1

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "freq_scheduler.h"
#include "txn_manager.h"
#include "util.h"
#include "scheduler.h"

static void
freq_scheduler_set_error(FreqScheduler *sch, int code, const char *message) {
     error_set(sch->error, code, message);
}

static void
freq_scheduler_add_error(FreqScheduler *sch, const char *message) {
     error_add(sch->error, message);
}

FreqSchedulerError
freq_scheduler_new(FreqScheduler **sch, PageDB *db, const char *path) {
     FreqScheduler *p = *sch = malloc(sizeof(*p));
     if (p == 0)
          return freq_scheduler_error_memory;

     p->error = error_new();
     if (p->error == 0) {
          free(p);
          return freq_scheduler_error_memory;
     }

     p->page_db = db;
     p->persist = FREQ_SCHEDULER_DEFAULT_PERSIST;
     p->margin = -1.0; // disabled
     p->max_n_crawls = 0;

     // create directory if not present yet
     char *error = 0;
     p->path = path? strdup(path): concat(db->path, "freqs", '_');
     if (!p->path)
          error = "building scheduler path";
     else
          error = make_dir(p->path);

     if (error != 0) {
          freq_scheduler_set_error(p, freq_scheduler_error_invalid_path, __func__);
          freq_scheduler_add_error(p, error);
          return p->error->code;
     }

     if (txn_manager_new(&p->txn_manager, 0) != 0) {
          freq_scheduler_set_error(p, freq_scheduler_error_internal, __func__);
          freq_scheduler_add_error(p, p->txn_manager?
                                   p->txn_manager->error->message:
                                   "NULL");
          return p->error->code;
     }

     int rc;
     // initialize LMDB on the directory
     if ((rc = mdb_env_create(&p->txn_manager->env) != 0))
          error = "creating environment";
     else if ((rc = mdb_env_set_mapsize(p->txn_manager->env,
                                        FREQ_SCHEDULER_DEFAULT_SIZE)) != 0)
          error = "setting map size";
     else if ((rc = mdb_env_set_maxdbs(p->txn_manager->env, 1)) != 0)
          error = "setting number of databases";
     else if ((rc = mdb_env_open(
                    p->txn_manager->env,
                    p->path,
                    MDB_NOTLS | MDB_NOSYNC, 0664) != 0))
          error = "opening environment";

     if (error != 0) {
          freq_scheduler_set_error(p, freq_scheduler_error_internal, __func__);
          freq_scheduler_add_error(p, error);
          freq_scheduler_add_error(p, mdb_strerror(rc));
          return p->error->code;
     }

     return p->error->code;
}

FreqSchedulerError
freq_scheduler_cursor_open(FreqScheduler *sch, MDB_cursor **cursor) {
     char *error1 = 0;
     char *error2 = 0;

     MDB_txn *txn = 0;
     if (txn_manager_begin(sch->txn_manager, 0, &txn) != 0) {
          error1 = "starting transaction";
          error2 = sch->txn_manager->error->message;
          goto on_error;
     }

     MDB_dbi dbi;
     int mdb_rc =
          mdb_dbi_open(txn, "schedule", MDB_CREATE, &dbi) ||
          mdb_set_compare(txn, dbi, schedule_entry_mdb_cmp_asc) ||
          mdb_cursor_open(txn, dbi, cursor);

     if (mdb_rc != 0) {
          *cursor = 0;

	  error1 = "opening cursor";
	  error2 = mdb_strerror(mdb_rc);
     }

     return sch->error->code;

on_error:
     if (txn != 0)
          txn_manager_abort(sch->txn_manager, txn);
     freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
     freq_scheduler_add_error(sch, error1);
     freq_scheduler_add_error(sch, error2);
     return sch->error->code;
}

FreqSchedulerError
freq_scheduler_cursor_commit(FreqScheduler *sch, MDB_cursor *cursor) {
     MDB_txn *txn = mdb_cursor_txn(cursor);

     if (txn_manager_commit(sch->txn_manager, txn) != 0) {
	  if (txn != 0)
	       txn_manager_abort(sch->txn_manager, txn);
	  freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
	  freq_scheduler_add_error(sch, "commiting schedule transaction");
	  freq_scheduler_add_error(sch, sch->txn_manager->error->message);
     }
     return sch->error->code;
}

void
freq_scheduler_cursor_abort(FreqScheduler *sch, MDB_cursor *cursor) {
     if (cursor) {
	  txn_manager_abort(sch->txn_manager, mdb_cursor_txn(cursor));
     }
}

FreqSchedulerError
freq_scheduler_cursor_write(FreqScheduler *sch,
			    MDB_cursor *cursor,
			    uint64_t hash,
			    float freq) {
     if (freq <= 0)
	  return 0;

     ScheduleKey sk = {
	  .score = 0,
	  .hash  = hash
     };
     MDB_val key = {
	  .mv_size = sizeof(sk),
	  .mv_data = &sk,
     };

     MDB_val val = {
	  .mv_size = sizeof(float),
	  .mv_data = &freq,
     };
     int mdb_rc;
     if ((mdb_rc = mdb_cursor_put(cursor, &key, &val, 0)) != 0) {
	  freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
	  freq_scheduler_add_error(sch, "adding page to schedule");
	  freq_scheduler_add_error(sch, mdb_strerror(mdb_rc));
     }
     return sch->error->code;

}

FreqSchedulerError
freq_scheduler_load_simple(FreqScheduler *sch,
                           float freq_default,
                           float freq_scale) {
     char *error1 = 0;
     char *error2 = 0;
     MDB_cursor *cursor = 0;

     HashInfoStream *st;
     if (hashinfo_stream_new(&st, sch->page_db) != 0) {
          error1 = "creating stream";
          error2 = st? sch->page_db->error->message: "NULL";
          goto on_error;
     }

     if (freq_scheduler_cursor_open(sch, &cursor) != 0)
	  goto on_error;

     StreamState ss;
     uint64_t hash;
     PageInfo *pi;

     while ((ss = hashinfo_stream_next(st, &hash, &pi)) == stream_state_next) {
          if ((pi->n_crawls > 0) &&
	      ((sch->max_n_crawls == 0) || (pi->n_crawls < sch->max_n_crawls)) &&
	      !page_info_is_seed(pi)){

               float freq = freq_default;
               if (freq_scale > 0) {
                    float rate = page_info_rate(pi);
                    if (rate > 0) {
                         freq = freq_scale * rate;
                    }
               }
	       if (freq_scheduler_cursor_write(sch, cursor, hash, freq) != 0)
		    goto on_error;
          }
          page_info_delete(pi);
     }
     if (ss != stream_state_end) {
          error1 = "incorrect stream state";
          error2 = 0;
          hashinfo_stream_delete(st);
          goto on_error;
     }
     hashinfo_stream_delete(st);

     if (freq_scheduler_cursor_commit(sch, cursor) != 0)
	  goto on_error;

     return sch->error->code;

on_error:
     freq_scheduler_cursor_abort(sch, cursor);

     freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
     freq_scheduler_add_error(sch, error1);
     freq_scheduler_add_error(sch, error2);

     return sch->error->code;
}

FreqSchedulerError
freq_scheduler_load_mmap(FreqScheduler *sch, MMapArray *freqs) {
     char *error1 = 0;
     char *error2 = 0;
     MDB_cursor *cursor = 0;

     if (txn_manager_expand(
              sch->txn_manager,
              2*freqs->n_elements*freqs->element_size) != 0) {
          error1 = "resizing database";
          error2 = sch->txn_manager->error->message;
          goto on_error;
     }

     if (freq_scheduler_cursor_open(sch, &cursor) != 0)
	  goto on_error;

     for (size_t i=0; i<freqs->n_elements; ++i) {
          PageFreq *f = mmap_array_idx(freqs, i);
          ScheduleKey sk = {
               .score = 1.0/f->freq,
               .hash = f->hash
          };
          MDB_val key = {
               .mv_size = sizeof(sk),
               .mv_data = &sk,
          };
          MDB_val val = {
               .mv_size = sizeof(float),
               .mv_data = &f->freq,
          };
	  int mdb_rc;
          if ((mdb_rc = mdb_cursor_put(cursor, &key, &val, 0)) != 0) {
               error1 = "adding page to schedule";
               error2 = mdb_strerror(mdb_rc);
               goto on_error;
          }
     }
     if (freq_scheduler_cursor_commit(sch, cursor) != 0)
	  goto on_error;

     return sch->error->code;

on_error:
     freq_scheduler_cursor_abort(sch, cursor);

     freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
     freq_scheduler_add_error(sch, error1);
     freq_scheduler_add_error(sch, error2);

     return sch->error->code;
}

FreqSchedulerError
freq_scheduler_request(FreqScheduler *sch,
                       size_t max_requests,
                       PageRequest **request) {
     char *error1 = 0;
     char *error2 = 0;

     MDB_cursor *cursor = 0;

     if (freq_scheduler_cursor_open(sch, &cursor) != 0)
	  goto on_error;

     PageRequest *req = *request = page_request_new(max_requests);
     if (!req) {
          error1 = "allocating memory";
          goto on_error;
     }

     int interrupt_requests = 0;
     while ((req->n_urls < max_requests) && !interrupt_requests) {
          MDB_val key;
          MDB_val val;
          ScheduleKey sk;
          float freq;
	  int mdb_rc;

	  int crawl = 0;
          switch (mdb_rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST)) {
          case 0:
	       // copy data before deleting cursor
               sk = *(ScheduleKey*)key.mv_data;
               freq = *(float*)val.mv_data;


               PageInfo *pi = 0;
               if (page_db_get_info(sch->page_db, sk.hash, &pi) != 0) {
                    error1 = "retrieving PageInfo from PageDB";
                    error2 = sch->page_db->error->message;
                    goto on_error;
               }

               if (pi) {
                    if (sch->margin >= 0) {
                         double elapsed = difftime(time(0), 0) - pi->last_crawl;
                         if (elapsed < 1.0/(freq*(1.0 + sch->margin)))
                              interrupt_requests = 1;
                    }
		    crawl = (sch->max_n_crawls == 0) || (pi->n_crawls < sch->max_n_crawls);
	       }
	       if (!interrupt_requests) {
		    if ((mdb_rc = mdb_cursor_del(cursor, 0)) != 0) {
			 error1 = "deleting head of schedule";
			 error2 = mdb_strerror(mdb_rc);
			 goto on_error;
		    }
		    if (crawl) {
			 if (page_request_add_url(req, pi->url) != 0) {
			      error1 = "adding url to request";
			      goto on_error;
			 }

			 sk.score += 1.0/freq;

			 val.mv_data = &freq;
			 key.mv_data = &sk;
			 if ((mdb_rc = mdb_cursor_put(cursor, &key, &val, 0)) != 0) {
			      error1 = "moving element inside schedule";
			      error2 = mdb_strerror(mdb_rc);
			      goto on_error;
			 }
		    }
	       }
	       page_info_delete(pi);

               break;

          case MDB_NOTFOUND: // no more pages left
               interrupt_requests = 1;
               break;
          default:
               error1 = "getting head of schedule";
               error2 = mdb_strerror(mdb_rc);
               goto on_error;
          }
     }
     if (freq_scheduler_cursor_commit(sch, cursor) != 0)
	  goto on_error;

     return sch->error->code;
on_error:
     freq_scheduler_cursor_abort(sch, cursor);

     freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
     freq_scheduler_add_error(sch, error1);
     freq_scheduler_add_error(sch, error2);

     return sch->error->code;
}

FreqSchedulerError
freq_scheduler_add(FreqScheduler *sch, const CrawledPage *page) {
     if (page_db_add(sch->page_db, page, 0) != 0) {
          freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
          freq_scheduler_add_error(sch, "adding crawled page");
          freq_scheduler_add_error(sch, sch->page_db->error->message);
     }
     return sch->error->code;
}

void
freq_scheduler_delete(FreqScheduler *sch) {
     mdb_env_close(sch->txn_manager->env);
     (void)txn_manager_delete(sch->txn_manager);
     if (!sch->persist) {
          char *data = build_path(sch->path, "data.mdb");
          char *lock = build_path(sch->path, "lock.mdb");
          remove(data);
          remove(lock);
          free(data);
          free(lock);

          remove(sch->path);
     }
     free(sch->path);
     error_delete(sch->error);
     free(sch);
}

FreqSchedulerError
freq_scheduler_dump(FreqScheduler *sch, FILE *output) {
     MDB_cursor *cursor;
     if (freq_scheduler_cursor_open(sch, &cursor) != 0)
	  return sch->error->code;

     int end = 0;
     MDB_cursor_op cursor_op = MDB_FIRST;
     do {
	  int mdb_rc;
	  MDB_val key;
	  MDB_val val;
	  ScheduleKey *key_data;
	  float *val_data;
	  switch (mdb_rc = mdb_cursor_get(cursor, &key, &val, cursor_op)) {
	  case 0:
	       key_data = (ScheduleKey*)key.mv_data;
	       val_data = (float*)val.mv_data;
	       fprintf(output, "%.2e %016"PRIx64" %.2e\n",
		       key_data->score, key_data->hash, *val_data);
	       break;
	  case MDB_NOTFOUND:
	       end = 1;
	       break;
	  default:
	       freq_scheduler_set_error(sch, freq_scheduler_error_internal, __func__);
	       freq_scheduler_add_error(sch, "iterating over database");
	       freq_scheduler_add_error(sch, mdb_strerror(mdb_rc));
	       end = 1;
	       break;
	  }
	  cursor_op = MDB_NEXT;
     } while (!end);
     freq_scheduler_cursor_abort(sch, cursor);

     return sch->error->code;
}
#if (defined TEST) && TEST
#include "test_freq_scheduler.c"
#endif // TEST
