def triangular_number(count):
    if not isinstance(count, int) or count < 0:
        raise ValueError("count must be a non-negative integer")
    return count * (count + 1) // 2
