/* Miner-only coinbase regression tests. Build with the same libs as smt-miner. */
#define main smt_miner_program_main
#include "smt-miner.c"
#undef main
#include <assert.h>

static json_t *template(void)
{
    json_error_t error;
    json_t *t = json_loads("{\"height\":149,\"version\":805306368,\"curtime\":1790441999,"
        "\"bits\":\"207fffff\",\"coinbasevalue\":1000,"
        "\"previousblockhash\":\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"target\":\"7fffff0000000000000000000000000000000000000000000000000000000000\","
        "\"masternode\":[],\"superblock\":[],\"transactions\":[]}", 0, &error);
    assert(t);
    return t;
}

static void set_json(json_t *t, const char *key, const char *value)
{
    json_error_t error;
    json_t *v = json_loads(value, JSON_DECODE_ANY, &error);
    assert(v);
    json_object_set_new(t, key, v);
}

static void check_outputs(json_t *t, const uint64_t *amounts, const uint8_t *scripts, size_t n)
{
    uint8_t spk[25] = {0};
    char err[512] = {0};
    block_t block = {0};
    int rc = build_block(t, spk, 1, &block, err, sizeof err);
    if (rc) fprintf(stderr, "build failed: %s\n", err);
    assert(rc == 0);
    /* One transaction; skip version, input outpoint, scriptSig and sequence. */
    const uint8_t *p = block.txs.p;
    assert(*p++ == 1);
    p += 4;
    assert(*p++ == 1);
    p += 36;
    size_t siglen = *p++;
    p += siglen + 4;
    assert(*p++ == n);
    for (size_t i = 0; i < n; ++i) {
        uint64_t amount = 0;
        for (int b = 0; b < 8; ++b) amount |= (uint64_t)*p++ << (8*b);
        assert(amount == amounts[i]);
        size_t len = *p++;
        assert(len == (i == 0 ? 25 : 1));
        if (i == 0) assert(!memcmp(p, spk, len));
        else assert(*p == scripts[i]);
        p += len;
    }
    assert(p + 4 == block.txs.p + block.txs.n);
    free(block.txs.p);
}

static void reject(const char *key, const char *value)
{
    json_t *t = template();
    set_json(t, key, value);
    block_t b = {0};
    uint8_t spk[25] = {0};
    char err[512] = {0};
    assert(build_block(t, spk, 1, &b, err, sizeof err) == -1);
    assert(*err);
    free(b.txs.p);
    json_decref(t);
}

int main(void)
{
    json_t *t = template();
    check_outputs(t, (uint64_t[]){1000}, (uint8_t[]){0}, 1);
    set_json(t, "masternode", "[{\"amount\":200,\"script\":\"51\"},{\"amount\":100,\"script\":\"6a\"}]");
    check_outputs(t, (uint64_t[]){700,200,100}, (uint8_t[]){0,0x51,0x6a}, 3);
    set_json(t, "superblock", "[{\"amount\":250,\"script\":\"52\"}]");
    check_outputs(t, (uint64_t[]){450,200,100,250}, (uint8_t[]){0,0x51,0x6a,0x52}, 4);
    json_decref(t);
    reject("masternode", "[{\"amount\":-1,\"script\":\"51\"}]");
    reject("masternode", "[{\"amount\":1001,\"script\":\"51\"}]");
    reject("masternode", "[{\"amount\":600,\"script\":\"51\"},{\"amount\":600,\"script\":\"51\"}]");
    reject("masternode", "[{\"amount\":1,\"script\":\"zz\"}]");
    reject("masternode", "[{\"amount\":1,\"script\":\"5\"}]");
    reject("masternode", "[{\"amount\":1,\"script\":\"\"}]");
    reject("masternode", "[{\"amount\":1.5,\"script\":\"51\"}]");
    reject("masternode", "[{\"script\":\"51\"}]");
    reject("masternode", "{}");
    reject("superblock", "[null]");
    reject("coinbasevalue", "-1");
    reject("coinbasevalue", "1.5");
    printf("{\"coinbase_tests\":\"PASS\",\"valid_cases\":3,\"invalid_cases\":12}\n");
    return 0;
}
