// Distinguish injected clipboard callbacks from new local clipboard events.
package com.clipsync;

public final class ClipboardEvents {
    private String remoteLabel;
    private String remoteText;
    private long lastTimestamp = Long.MIN_VALUE;
    private String lastText;

    public void applied(String label, String text) {
        remoteLabel = label;
        remoteText = text;
    }

    public boolean local(long timestamp, CharSequence label, String text) {
        if (remoteLabel != null && remoteLabel.contentEquals(label == null ? "" : label) && remoteText.equals(text)) {
            return false;
        }
        if (timestamp == lastTimestamp && text.equals(lastText)) {
            return false;
        }
        clear();
        lastTimestamp = timestamp;
        lastText = text;
        return true;
    }

    public void clear() {
        remoteLabel = null;
        remoteText = null;
        lastTimestamp = Long.MIN_VALUE;
        lastText = null;
    }
}
