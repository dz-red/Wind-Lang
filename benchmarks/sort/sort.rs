use std::time::Instant;

const SIZE: usize = 8192;

fn fill(values: &mut [i32; SIZE]) {
    let mut seed = 42;
    for item in values {
        seed = (seed * 25173 + 13849) % 65536;
        *item = seed;
    }
}

fn insertion_sort(values: &mut [i32; SIZE]) {
    for i in 1..SIZE {
        let value = values[i];
        let mut j = i;
        while j > 0 && values[j - 1] > value {
            values[j] = values[j - 1];
            j -= 1;
        }
        values[j] = value;
    }
}

fn digest(values: &[i32; SIZE]) -> i32 {
    let mut hash = 0;
    for &value in values {
        hash = (hash * 31 + value) % 65536;
    }
    hash
}

fn main() {
    let mut values = [0; SIZE];
    for _ in 0..2 {
        fill(&mut values);
        insertion_sort(&mut values);
    }
    fill(&mut values);
    let started = Instant::now();
    insertion_sort(&mut values);
    let elapsed_ms = started.elapsed().as_secs_f64() * 1000.0;
    println!("{elapsed_ms:.6} {}", digest(&values));
}
