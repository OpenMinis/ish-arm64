"""Pure-Python CPU kernels: n-body, fannkuch, spectral norm."""
import sys


def nbody(steps):
    pi, days = 3.141592653589793, 365.24
    sm = 4 * pi * pi
    bodies = [
        [[0.0, 0.0, 0.0], [0.0, 0.0, 0.0], sm],
        [[4.84, -1.16, -0.10], [0.606 * days, 2.81 * days, -0.02 * days], 9.5e-4 * sm],
        [[8.34, 4.12, -0.40], [-1.01 * days, 1.82 * days, 0.008 * days], 2.85e-4 * sm],
        [[12.89, -15.11, -0.22], [1.08 * days, 0.868 * days, -0.01 * days], 4.36e-5 * sm],
        [[15.37, -25.91, 0.17], [0.979 * days, 0.594 * days, -0.034 * days], 5.15e-5 * sm],
    ]
    pairs = [(bodies[i], bodies[j]) for i in range(5) for j in range(i + 1, 5)]
    dt = 0.01
    for _ in range(steps):
        for ([x1, y1, z1], v1, m1), ([x2, y2, z2], v2, m2) in pairs:
            dx, dy, dz = x1 - x2, y1 - y2, z1 - z2
            mag = dt * ((dx * dx + dy * dy + dz * dz) ** -1.5)
            b1, b2 = m1 * mag, m2 * mag
            v1[0] -= dx * b2; v1[1] -= dy * b2; v1[2] -= dz * b2
            v2[0] += dx * b1; v2[1] += dy * b1; v2[2] += dz * b1
        for r, [vx, vy, vz], m in bodies:
            r[0] += dt * vx; r[1] += dt * vy; r[2] += dt * vz
    return round(bodies[0][0][0], 6)


def fannkuch(n):
    perm1 = list(range(n))
    count = [0] * n
    maxflips = checksum = 0
    r, permcount = n, 0
    while True:
        while r != 1:
            count[r - 1] = r
            r -= 1
        perm = perm1[:]
        flips = 0
        k = perm[0]
        while k:
            perm[:k + 1] = perm[k::-1]
            flips += 1
            k = perm[0]
        maxflips = max(maxflips, flips)
        checksum += flips if permcount % 2 == 0 else -flips
        while True:
            if r == n:
                return checksum, maxflips
            p0 = perm1[0]
            perm1[:r] = perm1[1:r + 1]
            perm1[r] = p0
            count[r] -= 1
            if count[r] > 0:
                break
            r += 1
        permcount += 1


def spectral(n):
    def a(i, j):
        return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)

    def av(u):
        return [sum(a(i, j) * u[j] for j in range(n)) for i in range(n)]

    def atv(u):
        return [sum(a(j, i) * u[j] for j in range(n)) for i in range(n)]

    u = [1.0] * n
    for _ in range(5):
        v = atv(av(u))
        u = atv(av(v))
    return round(sum(x * y for x, y in zip(u, v)) / sum(x * x for x in v), 9)


which = sys.argv[1]
if which == 'nbody':
    print(nbody(60000))
elif which == 'fannkuch':
    print(fannkuch(9))
elif which == 'spectral':
    print(spectral(200))
