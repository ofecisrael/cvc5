#!/usr/bin/env python3
import fcntl
import sys



def increment_global_file(file, add):
    with open(file, 'a+') as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        try:
            current = int(f.read().strip() or '0')
            f.seek(0)
            f.truncate()
            f.write(str(current + add))
        finally:
            fcntl.flock(f, fcntl.LOCK_UN)


def main():
    args = []
    for line in sys.stdin:
        args.append(line.strip())
        if len(args) == 4:
            break
    with open('/tmp/args', 'a') as f:
        f.write(' '.join(args) + '\n')
        
    # sys.exit(1)

    regex = args[1]
    # count re.comp and re.inter
    count_inter = regex.count('re.inter')
    increment_global_file('/tmp/inter', count_inter)
    coun_comp = regex.count('re.comp')
    increment_global_file('/tmp/comp', count_inter)

    b = args[2]
    print(b)

if __name__ == "__main__":
    main()
