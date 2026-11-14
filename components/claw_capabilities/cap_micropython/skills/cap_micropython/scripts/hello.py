import json
import sys

from totals import triangular_number


name = args.get("name", "ESP32-S3")
count = args.get("count", 4)
print(json.dumps({
    "runtime": sys.implementation.name,
    "greeting": "Hello, " + name,
    "sum": triangular_number(count),
}))
