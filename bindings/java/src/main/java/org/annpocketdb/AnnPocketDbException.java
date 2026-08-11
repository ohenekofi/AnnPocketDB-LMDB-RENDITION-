package org.annpocketdb;

/**
 * Exception thrown when a native AnnPocketDb operation fails.
 */
public class AnnPocketDbException extends RuntimeException {
    private final int code;

    public AnnPocketDbException(int code, String message) {
        super("annpocketdb[" + code + "]: " + message);
        this.code = code;
    }

    public int getCode() {
        return code;
    }
}
