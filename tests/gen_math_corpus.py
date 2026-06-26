#!/usr/bin/env python3
import math
import sys


def cfmt(value):
    if math.isnan(value):
        return "nan"
    if math.isinf(value):
        return "inf" if value > 0 else "-inf"
    rounded = round(value)
    if abs(value - rounded) < 1e-10 and abs(value) < 1e15:
        return str(int(rounded))
    return "%.15g" % value


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else "tests/math_corpus.tsv"
    rows = []

    def mathf(expr, value):
        rows.append(("mathf", expr, cfmt(float(value))))

    def mathf_raw(expr, value):
        rows.append(("mathf", expr, value))

    def llm(expr):
        rows.append(("llm", expr, "-"))

    for a in range(-30, 31):
        for b in range(-30, 31):
            mathf(f"({a})+({b})", a + b)
            mathf(f"({a})-({b})", a - b)
            mathf(f"({a})*({b})", a * b)

    for a in range(-40, 41):
        for b in range(1, 21):
            mathf(f"({a})/({b})", a / b)

    for n in range(0, 101):
        mathf(f"sqrt({n * n})", n)
        mathf(f"sqrt {n * n}", n)
        mathf(f"abs({-n})", n)
        mathf(f"floor({n}.75)", n)
        mathf(f"ceil({n}.25)", n + 1)

    for a in range(1, 31):
        for b in range(1, 31):
            mathf(f"min({a},{b})", min(a, b))
            mathf(f"max({a},{b})", max(a, b))

    for n in range(0, 16):
        mathf(f"fib({n})", [0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610][n])

    for a, b in [(2, 8), (3, 4), (5, 3), (9, 2)]:
        mathf(f"pow({a},{b})", math.pow(a, b))
    for value in [0, 1, 2, 10, 16, 31, 255]:
        mathf(f"0x{value:x}+1", value + 1)
        mathf(f"0b{value:b}+2", value + 2)
        mathf(f"0o{value:o}+3", value + 3)

    mathf("pi*2", math.pi * 2)
    mathf("tau/2", math.tau / 2)
    mathf("e+1", math.e + 1)
    mathf("phi*10", 1.6180339887498948 * 10)
    mathf("log(100)", 2)
    mathf("ln(e)", 1)
    mathf("log2(1024)", 10)
    mathf("hypot(3,4)", 5)
    mathf("fmod(10,3)", 1)
    mathf("clamp(10,0,5)", 5)
    mathf("deg(pi)", 180)
    mathf("rad(180)", math.pi)
    mathf_raw("hex(255)", "0xff")
    mathf_raw("bin(10)", "0b1010")
    mathf_raw("oct(8)", "0o10")
    mathf("dec(0x10)", 16)

    # bare hex/number with no op (current policy: route llm)
    for expr in [
        "42",
        "0x10",
        "0b1010",
        "1e2",
        "pi",
        "hello world",
        "what is 2+2",
        "sqrt happiness",
        "price / earnings ratio",
        "convert 5 dollars to yen",
    ]:
        llm(expr)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write("# route\texpr\texpected\n")
        for route, expr, expected in rows:
            f.write(f"{route}\t{expr}\t{expected}\n")


if __name__ == "__main__":
    main()
