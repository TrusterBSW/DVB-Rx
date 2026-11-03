# Source this file: . ./env.sh
_root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export PATH="$_root/prefix/bin:$PATH"
export LD_LIBRARY_PATH="$_root/prefix/lib:$_root/prefix/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH}"
export PYTHONPATH="$_root/prefix/lib/$(python3 -c "import sys; print(\"python%d.%d\" % sys.version_info[:2])")/site-packages:${PYTHONPATH}"
export GRC_BLOCKS_PATH="$_root/prefix/share/gnuradio/grc/blocks:${GRC_BLOCKS_PATH}"
unset _root
