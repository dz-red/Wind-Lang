import { performance } from "node:perf_hooks";

const SIZE = 8192;
const values = new Int32Array(SIZE);

function fill() {
    let seed = 42;
    for (let i = 0; i < SIZE; i++) {
        seed = (seed * 25173 + 13849) % 65536;
        values[i] = seed;
    }
}

function insertionSort() {
    for (let i = 1; i < SIZE; i++) {
        const value = values[i];
        let j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

function digest() {
    let hash = 0;
    for (const value of values) {
        hash = (hash * 31 + value) % 65536;
    }
    return hash;
}

for (let i = 0; i < 2; i++) {
    fill();
    insertionSort();
}
fill();
const started = performance.now();
insertionSort();
const elapsedMs = performance.now() - started;
console.log(`${elapsedMs.toFixed(6)} ${digest()}`);
