/**
 * Tests sharding specific functionality of the setUserWriteBlockMode command. Non sharding specific
 * aspects of this command should be checked on jstests/noPassthrough/set_user_write_block_mode.js
 * instead.
 *
 * @tags: [
 *   requires_fcv_60,
 *   featureFlagUserWriteBlocking,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

function removeShard(shardName) {
    assert.soon(function() {
        let res = assert.commandWorked(st.s.adminCommand({removeShard: shardName}));
        if (!res.ok && res.code === ErrorCodes.ShardNotFound) {
            // If the config server primary steps down right after removing the config.shards doc
            // for the shard but before responding with "state": "completed", the mongos would retry
            // the _configsvrRemoveShard command against the new config server primary, which would
            // not find the removed shard in its ShardRegistry if it has done a ShardRegistry reload
            // after the config.shards doc for the shard was removed. This would cause the command
            // to fail with ShardNotFound.
            return true;
        }
        return res.state == 'completed';
    });
}

const st = new ShardingTest({shards: 2});

const dbName = "test";
const collName = "foo";
const ns = dbName + "." + collName;

assert.commandWorked(
    st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

let db = st.s.getDB(dbName);
let coll = db[collName];

const newShardName = 'newShard';
const newShard = new ReplSetTest({name: newShardName, nodes: 1});
newShard.startSet({shardsvr: ''});
newShard.initiate();

// Test addShard sets the proper user writes blocking state on the new shard.
{
    // Create a collection on the new shard before adding it to the cluster.
    const newShardDB = 'newShardDB';
    const newShardColl = 'newShardColl';
    const newShardCollDirect = newShard.getPrimary().getDB(newShardDB).getCollection(newShardColl);
    assert.commandWorked(newShardCollDirect.insert({x: 1}));
    const newShardCollMongos = st.s.getDB(newShardDB).getCollection(newShardColl);

    // Start blocking user writes.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

    // Add a new shard.
    assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));

    // Check that we cannot write on the new shard.
    assert.commandFailedWithCode(newShardCollMongos.insert({x: 2}), ErrorCodes.OperationFailed);

    // Now unblock and check we can write to the new shard.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
    assert.commandWorked(newShardCollMongos.insert({x: 2}));

    // Block again and see we can remove the shard even when write blocking is enabled. Before
    // removing the shard we first need to drop the dbs for which 'newShard' is the db-primary
    // shard.
    assert.commandWorked(st.s.getDB(newShardDB).dropDatabase());
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));
    removeShard(newShardName);

    // Disable write blocking while 'newShard' is not part of the cluster.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));

    // Add the shard back and check that user write blocking is disabled.
    assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));
    assert.commandWorked(newShardCollDirect.insert({x: 10}));
}

// Test addShard serializes with setUserWriteBlockMode.
{
    // Start setUserWriteBlockMode and make it hang during the SetUserWriteBlockModeCoordinator
    // execution.
    let hangInShardsvrSetUserWriteBlockModeFailPoint =
        configureFailPoint(st.shard0, "hangInShardsvrSetUserWriteBlockMode");
    let awaitShell = startParallelShell(() => {
        assert.commandWorked(db.adminCommand({setUserWriteBlockMode: 1, global: true}));
    }, st.s.port);
    hangInShardsvrSetUserWriteBlockModeFailPoint.wait();

    assert.commandFailedWithCode(
        st.s.adminCommand({addShard: newShard.getURL(), name: newShardName, maxTimeMS: 1000}),
        ErrorCodes.MaxTimeMSExpired);

    hangInShardsvrSetUserWriteBlockModeFailPoint.off();
    awaitShell();

    assert.commandWorked(st.s.adminCommand({addShard: newShard.getURL(), name: newShardName}));
    assert.commandWorked(st.s.adminCommand({removeShard: newShardName}));

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
}

// Test chunk migrations work even if user writes are blocked
{
    coll.drop();
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    // Insert a document to the chunk that will be migrated to ensure that the recipient will at
    // least insert one document as part of the migration.
    coll.insert({_id: 1});

    // Create an index to check that the recipient, upon receiving its first chunk, can create it.
    const indexKey = {x: 1};
    assert.commandWorked(coll.createIndex(indexKey));

    // Start blocking user writes.
    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

    // Move one chunk to shard1.
    assert.commandWorked(st.s.adminCommand({split: ns, middle: {_id: 0}}));
    assert.commandWorked(
        st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

    assert.eq(1, coll.find({_id: 1}).itcount());
    assert.eq(1, st.shard1.getDB(dbName)[collName].find({_id: 1}).itcount());

    // Check that the index has been created on recipient.
    ShardedIndexUtil.assertIndexExistsOnShard(st.shard1, dbName, collName, indexKey);

    // Check that orphans are deleted from the donor.
    assert.soon(() => {
        return st.shard0.getDB(dbName)[collName].find({_id: 1}).itcount() === 0;
    });

    // Create an extra index on shard1. This index is not present in the shard0, thus shard1 will
    // drop it when it receives its first chunk.
    {
        // Leave shard1 without any chunk.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard0.shardName}));

        // Create an extra index on shard1.
        assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
        const extraIndexKey = {y: 1};
        assert.commandWorked(st.shard1.getDB(dbName)[collName].createIndex(extraIndexKey));
        assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: true}));

        // Move a chunk to shard1.
        assert.commandWorked(
            st.s.adminCommand({moveChunk: ns, find: {_id: 0}, to: st.shard1.shardName}));

        // Check the mismatched index was dropped.
        ShardedIndexUtil.assertIndexDoesNotExistOnShard(st.shard1, dbName, collName, extraIndexKey);
    }

    assert.commandWorked(st.s.adminCommand({setUserWriteBlockMode: 1, global: false}));
}

st.stop();
newShard.stopSet();
})();
