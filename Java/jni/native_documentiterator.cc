//
//  native_documentiterator.cc
//  CBForest
//
//  Created by Jens Alfke on 9/12/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "com_couchbase_cbforest_DocumentIterator.h"
#include "native_glue.hh"
#include "c4Database.h"

using namespace forestdb::jni;

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_initEnumerateAllDocs
        (JNIEnv *env, jobject self, jlong dbHandle, jstring jStartDocID, jstring jEndDocID)
{
    jstringSlice startDocID(env, jStartDocID);
    jstringSlice endDocID(env, jEndDocID);
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateAllDocs((C4Database*)dbHandle, startDocID, endDocID, NULL, &error);
    if (!e) {
        throwError(env, error);
        return 0;
    }
    return (jlong)e;
}
JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_initEnumerateChanges
        (JNIEnv *env, jobject self, jlong dbHandle, jlong since)
{
    C4Error error;
    C4DocEnumerator *e = c4db_enumerateChanges((C4Database*)dbHandle, since, NULL, &error);
    if (!e) {
        throwError(env, error);
        return 0;
    }
    return (jlong)e;
}

JNIEXPORT jlong JNICALL Java_com_couchbase_cbforest_DocumentIterator_nextDocumentHandle
(JNIEnv *env, jclass clazz, jlong handle)
{
    auto e = (C4DocEnumerator*)handle;
    if (!e)
        return 0;
    C4Error error;
    auto doc = c4enum_nextDocument(e, &error);
    if (!doc) {
        if (error.code == 0)
            c4enum_free(e);  // automatically free at end, to save a JNI call to free()
        else
            throwError(env, error);
    }
    return (jlong)doc;
}


JNIEXPORT void JNICALL Java_com_couchbase_cbforest_DocumentIterator_free
(JNIEnv *env, jclass clazz, jlong handle)
{
    c4enum_free((C4DocEnumerator*)handle);
}