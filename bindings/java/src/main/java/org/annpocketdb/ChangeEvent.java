package org.annpocketdb;

public class ChangeEvent {
    public final long sequence;
    public final int type;
    public final long txnId;
    public final byte[] key;
    public final byte[] oldValue;
    public final byte[] newValue;

    public ChangeEvent(long sequence, int type, long txnId, byte[] key, byte[] oldValue, byte[] newValue) {
        this.sequence = sequence;
        this.type = type;
        this.txnId = txnId;
        this.key = key;
        this.oldValue = oldValue;
        this.newValue = newValue;
    }
}
