// Regression checks for repeated injected callbacks and deliberate local copies.
package com.clipsync;

public final class ClipboardEventsTest {
    private static void check(boolean value) {
        if (!value) throw new AssertionError("clipboard event classification");
    }

    public static void main(String[] args) {
        ClipboardEvents events = new ClipboardEvents();
        check(events.local(1, "app", "one"));
        check(!events.local(1, "app", "one"));
        check(events.local(2, "app", "one"));
        events.applied("ClipSync/0000000100000001", "remote\n😀");
        for (int count = 0; count < 1000; ++count) {
            check(!events.local(3, "ClipSync/0000000100000001", "remote\n😀"));
        }
        check(events.local(4, "app", "remote\n😀"));
        check(!events.local(4, "app", "remote\n😀"));
        check(events.local(5, "app", "remote\n😀"));
        events.applied("ClipSync/0000000100000002", "later");
        check(!events.local(6, "ClipSync/0000000100000002", "later"));
        check(events.local(7, "ClipSync/0000000100000002", "edited"));
        events.clear();
        check(events.local(7, "app", "edited"));
        check(events.local(8, null, "plain"));
        System.out.println("clipboard event tests ok");
    }
}
