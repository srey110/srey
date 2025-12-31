#include "task_mongo.h"

static int32_t _prt = 1;
static mongo_ctx _mongo;
static bson_ctx _comment;

static int32_t _test_insert() {
    char oid[BSON_OID_LENS];
    bson_ctx students;
    bson_init(&students, NULL, 0);
        bson_append_document_begain(&students, "0");
            bson_oid(oid);
            bson_append_oid(&students, "_id", oid);
            bson_append_utf8(&students, "name", "zhangsan");
            bson_append_int32(&students, "age", randrange(10, 15));
            bson_append_utf8(&students, "sex", "man");
        bson_append_end(&students);//0
        bson_append_document_begain(&students, "1");
            bson_oid(oid);
            bson_append_oid(&students, "_id", oid);
            bson_append_utf8(&students, "name", "lisi");
            bson_append_int32(&students, "age", randrange(10, 15));
            bson_append_utf8(&students, "sex", "woman");
        bson_append_end(&students);//1
    bson_append_end(&students);
    int32_t rtn = mongo_insert(&_mongo, students.doc.data, students.doc.offset, _comment.doc.data);
    BSON_FREE(&students);
    return rtn;
}
static int32_t _test_update(const char *name) {
    bson_ctx updates;
    bson_init(&updates, NULL, 0);
        bson_append_document_begain(&updates, "0");
            bson_append_document_begain(&updates, "q");
                bson_append_utf8(&updates, "name", name);
            bson_append_end(&updates);//q
            bson_append_document_begain(&updates, "u");
                bson_append_document_begain(&updates, "$inc");
                    bson_append_int32(&updates, "age", 1);
                bson_append_end(&updates);//$inc
            bson_append_end(&updates);//u
            bson_append_bool(&updates, "multi", 1);
        bson_append_end(&updates);//0
    bson_append_end(&updates);
    int32_t rtn = mongo_update(&_mongo, updates.doc.data, updates.doc.offset, _comment.doc.data);
    BSON_FREE(&updates);
    return rtn;
}
static int32_t _test_delete(const char *name) {
    bson_ctx deletes;
    bson_init(&deletes, NULL, 0);
        bson_append_document_begain(&deletes, "0");
            bson_append_document_begain(&deletes, "q");
                bson_append_utf8(&deletes, "name", name);
            bson_append_end(&deletes);//q
            bson_append_bool(&deletes, "limit", 0);
        bson_append_end(&deletes);//0
    bson_append_end(&deletes);
    int32_t rtn = mongo_delete(&_mongo, deletes.doc.data, deletes.doc.offset, _comment.doc.data);
    BSON_FREE(&deletes);
    return rtn;
}
static int32_t _test_find(const char *name) {
    mgopack_ctx *mgpack = mongo_find(&_mongo, NULL, 0, _comment.doc.data);
    if (NULL == mgpack) {
        return ERR_FAILED;
    }
    int64_t id = mongo_cursorid(mgpack);
    if (id > 0) {
        mgpack = mongo_getmore(&_mongo, id, _comment.doc.data);
        if (NULL == mgpack) {
            return ERR_FAILED;
        }
        bson_ctx kill;
        bson_init(&kill, NULL, 0);
        bson_append_int64(&kill, "0", id);
        bson_append_end(&kill);
        mgpack = mongo_killcursors(&_mongo, BSON_DOC(&kill), BSON_DOC_LENS(&kill), BSON_DOC(&_comment));
        BSON_FREE(&kill);
        if (NULL == mgpack) {
            return ERR_FAILED;
        }
    }
    mgpack = mongo_distinct(&_mongo, "name", NULL, 0, _comment.doc.data);
    if (NULL == mgpack) {
        return ERR_FAILED;
    }
    bson_ctx q;
    bson_init(&q, NULL, 0);
    bson_append_utf8(&q, "name", name);
    bson_append_end(&q);
    mgpack = mongo_findandmodify(&_mongo, q.doc.data, q.doc.offset, 1, 0, NULL, 0, _comment.doc.data);
    if (NULL == mgpack) {
        BSON_FREE(&q);
        return ERR_FAILED;
    }
    bson_ctx pp;
    bson_init(&pp, NULL, 0);
        bson_append_document_begain(&pp, "0");
            bson_append_document(&pp, "$match", q.doc.data, q.doc.offset);
        bson_append_end(&pp);//0
    bson_append_end(&pp);
    mgpack = mongo_aggregate(&_mongo, pp.doc.data, pp.doc.offset, _comment.doc.data);
    BSON_FREE(&q);
    BSON_FREE(&pp);
    return NULL == mgpack ? ERR_FAILED : ERR_OK;
}
static int32_t _test_bulkwrite(const char *name) {
    bson_ctx updates;
    bson_init(&updates, NULL, 0);
        bson_append_document_begain(&updates, "0");
            bson_append_int32(&updates, "update", 0);
            bson_append_document_begain(&updates, "filter");
                bson_append_utf8(&updates, "name", name);
            bson_append_end(&updates);//filter
            bson_append_document_begain(&updates, "updateMods");
                bson_append_document_begain(&updates, "$inc");
                    bson_append_int32(&updates, "age", 2);
                bson_append_end(&updates);//$inc
            bson_append_end(&updates);//updateMods
        bson_append_end(&updates);//0
    bson_append_end(&updates);
    bson_ctx ns;
    bson_init(&ns, NULL, 0);
        bson_append_document_begain(&ns, "0");
            char *dbco = format_va("%s.%s", _mongo.db, _mongo.collection);
            bson_append_utf8(&ns, "ns", dbco);
            FREE(dbco);
        bson_append_end(&ns);
    bson_append_end(&ns);
    mgopack_ctx *mgpack = mongo_bulkwrite(&_mongo, updates.doc.data, updates.doc.offset, ns.doc.data, ns.doc.offset, _comment.doc.data);
    BSON_FREE(&ns);
    BSON_FREE(&updates);
    return NULL == mgpack ? ERR_FAILED : ERR_OK;
}
static int32_t _test_indexes() {
    bson_ctx indexes;
    bson_init(&indexes, NULL, 0);
        bson_append_document_begain(&indexes, "0");
            bson_append_document_begain(&indexes, "key");
                bson_append_int32(&indexes, "age", 1);
            bson_append_end(&indexes);//key
            bson_append_utf8(&indexes, "name", "age_index");
        bson_append_end(&indexes);//0
    bson_append_end(&indexes);
    int32_t rtn = mongo_createindexes(&_mongo, indexes.doc.data, indexes.doc.offset, _comment.doc.data);
    BSON_FREE(&indexes);
    if (ERR_OK != rtn) {
        return ERR_FAILED;
    }
    bson_ctx indexname;
    bson_init(&indexname, NULL, 0);
    bson_append_utf8(&indexname, "0", "age_index");
    bson_append_end(&indexname);
    rtn = mongo_dropindexes(&_mongo, indexname.doc.data, indexname.doc.offset, _comment.doc.data);
    BSON_FREE(&indexname);
    return rtn;
}
static void _startup(task_ctx *task) {
    int32_t rtn;
    mongo_init(&_mongo, "127.0.0.1", 0, NULL, NULL);
    if (ERR_OK != mongo_connect(task, &_mongo)){
        LOG_ERROR("mongo_connect error.");
        return;
    }
    bson_init(&_comment, NULL, 0);
    bson_append_utf8(&_comment, "comment", "srey");
    bson_append_end(&_comment);

    mongo_db(&_mongo, "admin");
    bson_ctx saslsm;
    bson_init(&saslsm, NULL, 0);
    bson_append_utf8(&saslsm, "saslSupportedMechs", "admin.admin");
    bson_append_end(&saslsm);
    mgopack_ctx *mgpack = mongo_hello(&_mongo, saslsm.doc.data);
    BSON_FREE(&saslsm);
    if (ERR_OK != mongo_auth(&_mongo, "SCRAM-SHA-256", "admin", "12345678")) {
        LOG_ERROR("mongo_auth error.");
        return;
    }
    if (ERR_OK != mongo_auth(&_mongo, "SCRAM-SHA-1", "admin", "12345678")) {
        LOG_ERROR("mongo_auth error.");
        return;
    }
    mongo_collection(&_mongo, "test_collt");
    if (ERR_OK != mongo_ping(&_mongo)) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }

    mongo_set_flag(&_mongo, MORETOCOME);
    for (int i = 0; i < 100; i++) {
        if (ERR_FAILED == _test_insert()) {
            LOG_ERROR("%s", mongo_error(&_mongo));
            return;
        }
    }
    mongo_clear_flag(&_mongo);
    if (ERR_OK != _test_indexes()) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }
    if (ERR_FAILED == _test_update("zhangsan")) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }
    if (ERR_OK != _test_find("zhangsan")) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }
    if (ERR_OK != _test_bulkwrite("lisi")) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }
    rtn = mongo_count(&_mongo, NULL, 0, _comment.doc.data);
    if (ERR_FAILED == rtn) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }
    if (ERR_FAILED == _test_delete("lisi")) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }
    rtn = mongo_drop(&_mongo, _comment.doc.data);
    if (ERR_OK != rtn) {
        LOG_ERROR("%s", mongo_error(&_mongo));
        return;
    }
    LOG_INFO("mongo tested.");
}
static void _closing_cb(task_ctx *task) {
    BSON_FREE(&_comment);
}
void task_mongo_start(loader_ctx *loader, name_t name, int32_t pt) {
    _prt = pt;
    coro_task_register(loader, name, _startup, _closing_cb);
}
