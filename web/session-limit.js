// session-limit.js
// Allows one account to be logged in on at most 4 devices/tabs at the same time.
// Put this file in the same folder as firebase-config.js.

import { db, auth } from "./firebase-config.js";

import {
    doc,
    runTransaction
} from "https://www.gstatic.com/firebasejs/11.0.2/firebase-firestore.js";

import {
    onAuthStateChanged,
    signOut
} from "https://www.gstatic.com/firebasejs/11.0.2/firebase-auth.js";

export const MAX_SESSIONS = 4;

// A session that has sent no heartbeat for this long is treated as closed.
const STALE_MS = 90000;      // 90 seconds

// How often an open page tells Firestore "I am still here".
const BEAT_MS = 30000;       // 30 seconds

export const LIMIT_MESSAGE =
    "Maximum login limit reached (4 users). " +
    "Log out on another device and try again.";


// Each browser tab gets its own session id.
function getSessionId() {
    let id = sessionStorage.getItem("sessionId");

    if (!id) {
        id = "s" +
            Date.now().toString(36) +
            Math.random().toString(36).slice(2, 10);

        sessionStorage.setItem("sessionId", id);
    }

    return id;
}


// Claims a slot, or refreshes the slot this tab already has.
// Returns { ok: true } or { ok: false, reason: "limit" | "error" | "not-signed-in" }
export async function claimSession() {
    const user = auth.currentUser;

    if (!user) {
        return { ok: false, reason: "not-signed-in" };
    }

    const ref = doc(db, "activeSessions", user.uid);
    const id = getSessionId();

    try {
        return await runTransaction(db, async function (tx) {
            const snap = await tx.get(ref);
            const now = Date.now();

            const saved = snap.exists()
                ? (snap.data().sessions || {})
                : {};

            // Keep only OTHER sessions that are still alive.
            const active = {};

            Object.keys(saved).forEach(function (key) {
                const lastSeen = saved[key];

                if (
                    key !== id &&
                    typeof lastSeen === "number" &&
                    now - lastSeen <= STALE_MS
                ) {
                    active[key] = lastSeen;
                }
            });

            if (Object.keys(active).length >= MAX_SESSIONS) {
                return { ok: false, reason: "limit" };
            }

            active[id] = now;
            tx.set(ref, { sessions: active });

            return { ok: true };
        });

    } catch (error) {
        console.error("Session check failed:", error);
        return { ok: false, reason: "error" };
    }
}


// Frees this tab's slot (call this when the user logs out).
export async function releaseSession() {
    const user = auth.currentUser;
    const id = sessionStorage.getItem("sessionId");

    if (!user || !id) {
        return;
    }

    const ref = doc(db, "activeSessions", user.uid);

    try {
        await runTransaction(db, async function (tx) {
            const snap = await tx.get(ref);

            if (!snap.exists()) {
                return;
            }

            const sessions = Object.assign({}, snap.data().sessions || {});
            delete sessions[id];

            tx.set(ref, { sessions: sessions });
        });

    } catch (error) {
        console.error("Could not release session:", error);
    }

    sessionStorage.removeItem("sessionId");
}


// Call once on every protected page (dashboard, history, reports).
// Keeps this tab's slot alive. If the slot was lost and the limit is
// full, the user is signed out and sent back to the login page.
export function startSessionHeartbeat() {
    if (sessionStorage.getItem("loggedIn") !== "true") {
        return;
    }

    let timer = null;

    onAuthStateChanged(auth, function (user) {
        if (timer) {
            clearInterval(timer);
            timer = null;
        }

        if (!user) {
            return;
        }

        async function beat() {
            const result = await claimSession();

            if (!result.ok && result.reason === "limit") {
                clearInterval(timer);

                sessionStorage.removeItem("loggedIn");
                sessionStorage.removeItem("username");
                sessionStorage.removeItem("sessionId");

                await signOut(auth);

                alert(LIMIT_MESSAGE);
                window.location.href = "index.html";
            }
        }

        beat();
        timer = setInterval(beat, BEAT_MS);
    });
}
