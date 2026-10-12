# Pure-Python loops: arithmetic, branches, calls.
def collatz(n):
    steps = 0
    while n != 1:
        n = n // 2 if n % 2 == 0 else 3 * n + 1
        steps += 1
    return steps

best = max(range(1, 60000), key=collatz)
total = 0
for i in range(300000):
    total += i * i % 7
print(best, total)
