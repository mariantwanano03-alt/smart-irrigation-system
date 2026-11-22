import { initializeApp } from "https://www.gstatic.com/firebasejs/11.0.2/firebase-app.js";
import { getFirestore } from "https://www.gstatic.com/firebasejs/11.0.2/firebase-firestore.js";

const firebaseConfig = {
  apiKey: "AIzaSyDV2C0Yl1Pi8U428l0WtJ8QRsEl6H5qWLE",
  authDomain: "smart-irrigation-system-ddd4e.firebaseapp.com",
  projectId: "smart-irrigation-system-ddd4e",
  storageBucket: "smart-irrigation-system-ddd4e.firebasestorage.app",
  messagingSenderId: "1060508432076",
  appId: "1:1060508432076:web:a6ac6f4b736803619cf469",
  measurementId: "G-QRGB1Y137X"
};

const app = initializeApp(firebaseConfig);

export const db = getFirestore(app);


import {
    getAuth
} from "https://www.gstatic.com/firebasejs/11.0.2/firebase-auth.js";

// Authentication
export const auth = getAuth(app);
