from time import perf_counter

SIZE = 8192


def fill(values):
    seed = 42
    for i in range(SIZE):
        seed = (seed * 25173 + 13849) % 65536
        values[i] = seed


def insertion_sort(values):
    for i in range(1, SIZE):
        value = values[i]
        j = i
        while j > 0 and values[j - 1] > value:
            values[j] = values[j - 1]
            j -= 1
        values[j] = value


def digest(values):
    checksum = 0
    for value in values:
        checksum = (checksum * 31 + value) % 65536
    return checksum


if __name__ == "__main__":
    values = [0] * SIZE
    for _ in range(2):
        fill(values)
        insertion_sort(values)
    fill(values)
    started = perf_counter()
    insertion_sort(values)
    elapsed_ms = (perf_counter() - started) * 1000
    print(f"{elapsed_ms:.6f} {digest(values)}")
