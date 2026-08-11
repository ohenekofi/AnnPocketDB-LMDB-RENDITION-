package org.annpocketdb;

import java.util.Iterator;
import java.util.NoSuchElementException;

public class ChangeFeedIterator implements Iterator<ChangeEvent>, AutoCloseable {
    private long handle;
    private ChangeEvent nextEvent;

    ChangeFeedIterator(long handle) {
        this.handle = handle;
        this.nextEvent = Native.changefeedNext(handle);
    }

    @Override
    public boolean hasNext() {
        return nextEvent != null;
    }

    @Override
    public ChangeEvent next() {
        if (nextEvent == null) throw new NoSuchElementException();
        ChangeEvent current = nextEvent;
        nextEvent = Native.changefeedNext(handle);
        return current;
    }

    @Override
    public void close() {
        if (handle != 0) {
            Native.changefeedClosePoll(handle);
            handle = 0;
            nextEvent = null;
        }
    }
}
