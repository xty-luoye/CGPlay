"""Read-only build gate against stale Python packages and native modules."""
import argparse
from pathlib import Path


def files(root):
    return {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()
        and '__pycache__' not in p.relative_to(root).parts}


def validate(source,destination):
    source=Path(source).resolve();destination=Path(destination).resolve()
    if source==destination or source in destination.parents or destination in source.parents:
        raise ValueError('Runtime source and destination must not overlap')
    if not (source/'python.exe').is_file() or not (source/'Lib').is_dir():
        raise ValueError('Runtime source is incomplete')
    if destination.exists() and not destination.is_dir():
        raise ValueError('Runtime destination is not a directory')
    expected=files(source);present=files(destination) if destination.exists() else set()
    obsolete=sorted(present-expected)
    if obsolete:
        raise ValueError('Refusing mixed runtime deployment: preserve and recreate the generated runtime/python folder before switching runtimes. Obsolete files include: '+', '.join(obsolete[:8]))
    return {'sourceFiles':len(expected),'destinationFiles':len(present),'obsoleteFiles':0}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',required=True);parser.add_argument('--destination',required=True)
    arguments=parser.parse_args()
    try:result=validate(arguments.source,arguments.destination)
    except (ValueError,OSError) as error:parser.exit(2,str(error)+'\n')
    print('Embedded runtime overlay validated:',result)


if __name__=='__main__':main()
