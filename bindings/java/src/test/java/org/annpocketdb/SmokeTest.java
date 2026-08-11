package org.annpocketdb;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;

import static org.junit.jupiter.api.Assertions.*;

public class SmokeTest {
    private Path dbPath;

    @BeforeEach
    public void setup() throws Exception {
        dbPath = Files.createTempDirectory("apdb_test");
    }

    @AfterEach
    public void teardown() throws Exception {
        Files.walk(dbPath)
            .sorted(Comparator.reverseOrder())
            .map(Path::toFile)
            .forEach(File::delete);
    }

    @Test
    public void testBasicPutGetDelete() {
        try (Env env = Env.open(dbPath.toString())) {
            env.put("hello".getBytes(), "world".getBytes());
            byte[] val = env.get("hello".getBytes());
            assertNotNull(val);
            assertArrayEquals("world".getBytes(), val);

            byte[] missing = env.get("missing".getBytes());
            assertNull(missing);

            env.delete("hello".getBytes());
            assertNull(env.get("hello".getBytes()));
        }
    }

    @Test
    public void testWriteTransaction() {
        try (Env env = Env.open(dbPath.toString())) {
            try (WriteTxn txn = env.beginWrite()) {
                txn.put("k1".getBytes(), "v1".getBytes());
                txn.put("k2".getBytes(), "v2".getBytes());
                
                byte[] val = txn.get("k1".getBytes());
                assertArrayEquals("v1".getBytes(), val);
                
                txn.commit();
            }
            
            assertArrayEquals("v2".getBytes(), env.get("k2".getBytes()));
        }
    }

    @Test
    public void testWriteTransactionAbort() {
        try (Env env = Env.open(dbPath.toString())) {
            try (WriteTxn txn = env.beginWrite()) {
                txn.put("ephemeral".getBytes(), "data".getBytes());
                txn.abort(); // explicit abort, though close() does this automatically
            }
            assertNull(env.get("ephemeral".getBytes()));
        }
    }

    @Test
    public void testReadCursor() {
        try (Env env = Env.open(dbPath.toString())) {
            try (WriteTxn wtxn = env.beginWrite()) {
                wtxn.put("a".getBytes(), "1".getBytes());
                wtxn.put("b".getBytes(), "2".getBytes());
                wtxn.put("c".getBytes(), "3".getBytes());
                wtxn.commit();
            }

            try (ReadTxn rtxn = env.beginRead();
                 Cursor cur = rtxn.openCursor()) {
                
                assertTrue(cur.first());
                assertArrayEquals("a".getBytes(), cur.key());
                assertArrayEquals("1".getBytes(), cur.value());

                assertTrue(cur.next());
                assertArrayEquals("b".getBytes(), cur.key());
                assertArrayEquals("2".getBytes(), cur.value());

                assertTrue(cur.next());
                assertTrue(cur.next() == false);
            }
        }
    }

    @Test
    public void test2PC() {
        try (Env env = Env.open(dbPath.toString())) {
            try (WriteTxn txn = env.beginWrite()) {
                txn.put("2pc_k".getBytes(), "2pc_v".getBytes());
                txn.prepare();
            }
            assertArrayEquals("2pc_v".getBytes(), env.get("2pc_k".getBytes()));
            env.rollbackPrepared();
        }
    }

    @Test
    public void testBlobStorage() {
        try (Env env = Env.open(dbPath.toString())) {
            byte[] blob = "large_blob_payload".getBytes();
            env.putBlob("b1".getBytes(), blob, 0);
            assertArrayEquals(blob, env.getBlob("b1".getBytes()));
            env.deleteBlob("b1".getBytes(), 0);
            assertNull(env.getBlob("b1".getBytes()));
        }
    }

    @Test
    public void testChangefeed() {
        try (Env env = Env.open(dbPath.toString(), Env.CHANGEFEED)) {
            env.put("cfk".getBytes(), "cfv".getBytes());
            env.delete("cfk".getBytes());

            try (ReadTxn rtxn = env.beginRead();
                 ChangeFeedIterator cf = env.openChangeFeed(rtxn, 0)) {
                
                int count = 0;
                while (cf.hasNext()) {
                    ChangeEvent ev = cf.next();
                    count++;
                    if (count == 1) {
                        assertArrayEquals("cfk".getBytes(), ev.key);
                        assertEquals(1, ev.type); // insert
                    } else if (count == 2) {
                        assertEquals(3, ev.type); // delete
                    }
                }
                assertEquals(2, count);
            }
        }
    }
}
