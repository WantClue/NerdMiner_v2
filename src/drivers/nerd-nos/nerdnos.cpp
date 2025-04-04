#include <Arduino.h>
#include <ArduinoJson.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../mining.h"
#include "adc.h"
#include "bm1366.h"
#include "mbedtls/sha256.h"
#include "nerdnos.h"
#include "serial.h"
#include "stratum.h"
#include "utils.h"

///////cgminer nonce testing
/* truediffone ==
 * 0x00000000FFFF0000000000000000000000000000000000000000000000000000
 */
static const double truediffone =
    26959535291011309493156476344723991336010898738574164086137773096960.0;

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

BM1366 *bm1366;

// Declare a FreeRTOS mutex
SemaphoreHandle_t dump_mutex;

char dump_buf[1024] = {0};

void dump_bytes(const char *label, uint8_t *data, int len) {
  xSemaphoreTake(dump_mutex, portMAX_DELAY);
  int ofs = 0;

  char *p = &dump_buf[0];
  int remaining = sizeof(dump_buf);
  int written = 0;

  // Write the label to the buffer
  written = snprintf(p, remaining, "%s ", label);
  remaining -= written;
  p += written;

  // Write the hex representation of each byte
  for (int i = 0; i < len; i++) {
    written =
        snprintf(p, remaining, "%02x ", data[i]); // Dereference `data` here
    remaining -= written;
    p += written;
  }

  // Print the buffer
  Serial.println(dump_buf);
  xSemaphoreGive(dump_mutex);
}

/* testing a nonce and return the diff - 0 means invalid */
double nerdnos_test_nonce_value(const bm_job_t *job, const uint32_t nonce,
                                const uint32_t rolled_version,
                                uint8_t hash_result[32]) {
  double d64, s64, ds;
  unsigned char header[80];

  // // TODO: use the midstate hash instead of hashing the whole header
  // uint32_t rolled_version = job->version;
  // for (int i = 0; i < midstate_index; i++) {
  //     rolled_version = increment_bitmask(rolled_version, job->version_mask);
  // }

  // copy data from job to header
  memcpy(header, &rolled_version, 4);
  memcpy(header + 4, job->prev_block_hash, 32);
  memcpy(header + 36, job->merkle_root, 32);
  memcpy(header + 68, &job->ntime, 4);
  memcpy(header + 72, &job->target, 4);
  memcpy(header + 76, &nonce, 4);

  unsigned char hash_buffer[32];

  dump_bytes("header test 0-39", header, 40);
  dump_bytes("header test 40-79", header+40, 40);

  // double hash the header
  mbedtls_sha256(header, 80, hash_buffer, 0);
  dump_bytes("header test 1st hash", hash_buffer, 32);

  mbedtls_sha256(hash_buffer, 32, hash_result, 0);
  dump_bytes("header test 2nd hash", hash_result, 32);

  d64 = truediffone;
  s64 = le256todouble(hash_result);
  ds = d64 / s64;

  return ds;
}

static void calculate_merkle_root_hash(const char *coinbase_tx, mining_job *job,
                                       char merkle_root_hash[65]) {
  size_t coinbase_tx_bin_len = strlen(coinbase_tx) / 2;
  uint8_t coinbase_tx_bin[coinbase_tx_bin_len];
  hex2bin(coinbase_tx, coinbase_tx_bin, coinbase_tx_bin_len);

  uint8_t both_merkles[64];
  uint8_t new_root[32];
  double_sha256_bin(coinbase_tx_bin, coinbase_tx_bin_len, new_root);
  memcpy(both_merkles, new_root, 32);

  for (size_t i = 0; i < job->merkle_branch_size; i++) {
    const char *m = job->merkle_branch[i].c_str();
    hex2bin(m, &both_merkles[32], 32);
    double_sha256_bin(both_merkles, 64, new_root);
    memcpy(both_merkles, new_root, 32);
  }
  bin2hex(both_merkles, 32, merkle_root_hash, 65);
}

// take a mining_notify struct with ascii hex strings and convert it to a bm_job
// struct
static void construct_bm_job(mining_job *job, const char *merkle_root,
                             uint32_t version_mask, bm_job_t *new_job) {
  new_job->version = strtoul(job->version.c_str(), NULL, 16);
  new_job->target = strtoul(job->nbits.c_str(), NULL, 16);
  new_job->ntime = strtoul(job->ntime.c_str(), NULL, 16);
  new_job->starting_nonce = 0;

  hex2bin(merkle_root, new_job->merkle_root, 32);

  // hex2bin(merkle_root, new_job.merkle_root_be, 32);
  swap_endian_words(merkle_root, new_job->merkle_root_be);
  reverse_bytes(new_job->merkle_root_be, 32);

  swap_endian_words(job->prev_block_hash.c_str(), new_job->prev_block_hash);

  hex2bin(job->prev_block_hash.c_str(), new_job->prev_block_hash_be, 32);
  reverse_bytes(new_job->prev_block_hash_be, 32);
}

void nerdnos_create_job(mining_subscribe *mWorker, mining_job *job,
                        bm_job_t *next_job, uint32_t extranonce_2,
                        uint32_t stratum_difficulty) {
  char extranonce_2_str[mWorker->extranonce2_size * 2 +
                        1]; // +1 zero termination
  snprintf(extranonce_2_str, sizeof(extranonce_2_str), "%0*lx",
           (int)mWorker->extranonce2_size * 2, extranonce_2);

  // generate coinbase tx
  String coinbase_tx =
      job->coinb1 + mWorker->extranonce1 + extranonce_2_str + job->coinb2;

  // calculate merkle root
  char merkle_root[65];
  calculate_merkle_root_hash(coinbase_tx.c_str(), job, merkle_root);

  // Serial.printf("asic merkle root: %s\n", merkle_root);
  construct_bm_job(job, merkle_root, 0x1fffe000, next_job);

  next_job->jobid = strdup(job->job_id.c_str());
  next_job->extranonce2 = strdup(extranonce_2_str);
  next_job->pool_diff = stratum_difficulty;
}

void nerdnos_send_work(bm_job_t *next_bm_job, uint8_t job_id) {
  bm1366->sendWork(job_id, next_bm_job);
}

bool nerdnos_proccess_work(uint16_t timeout,
                           task_result *result) {
  return bm1366->processWork(timeout, result);
}

void nerdnos_free_bm_job(bm_job_t *job) {
  free(job->jobid);
  free(job->extranonce2);
  // mark as free
  job->ntime = 0;
}

void nerdnos_set_asic_difficulty(uint32_t difficulty) {
  bm1366->setJobDifficultyMask(difficulty);
}

void nerdnos_init() {
  bm1366 = new BM1366();
  nerdnos_adc_init();
  SERIAL_init();
  int chips = bm1366->init(200, 1, 128);
  Serial.printf("found bm1397: %d\n", chips);
  int baud = bm1366->setMaxBaud();
  vTaskDelay(100 / portTICK_PERIOD_MS);
  SERIAL_set_baud(baud);
  vTaskDelay(100 / portTICK_PERIOD_MS);
}
