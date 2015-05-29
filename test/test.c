#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "CuTest.h"

#include "page_db.h"
#include "page_rank.h"
#include "hits.h"
#include "bf_scheduler.h"
#include "domain_temp.h"

int main(int argc, char **argv) {
     size_t n_pages = 0;
     char *end;
     switch (argc) {
     case 1:
          n_pages = 50000;
          break;
     case 2:
          n_pages = strtol(argv[1], &end, 10);
          if (*end != '\0') {
               fprintf(stderr, "Please enter a valid number as argument\n");
               goto on_error;
          }
          break;
     default:
          goto on_error;
     }

     int fail_count = 0;
#define RUN_SUITE(command) do{\
     CuString *output = CuStringNew();\
     CuSuite *suite = command;\
     CuSuiteRun(suite);\
     CuSuiteSummary(suite, output);\
     CuSuiteDetails(suite, output);\
     CuSuiteDelete(suite);\
     printf("%s\n", output->buffer);\
     CuStringDelete(output);\
     fail_count += suite->failCount;\
} while(0);

     RUN_SUITE(test_page_db_suite(n_pages));
     RUN_SUITE(test_page_rank_suite());
     RUN_SUITE(test_hits_suite());
     RUN_SUITE(test_bf_scheduler_suite(n_pages));
     RUN_SUITE(test_util_suite());
     RUN_SUITE(test_domain_temp_suite());
     if (fail_count == 0)
          return 0;
     else
          return -1;

on_error:
     fprintf(stderr, "\n");
     fprintf(stderr, "Usage: %s [n_pages]\n", argv[0]);
     return -1;
}
