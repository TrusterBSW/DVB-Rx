"""Python bindings (ctypes) for libdvbrx."""
import ctypes
import os

import numpy as np

_here = os.path.dirname(os.path.abspath(__file__))
_lib = ctypes.CDLL(os.environ.get(
    "DVBRX_LIB",
    os.path.join(_here, "..", "..", "libdvbrx", "build", "libdvbrx.so")))


class _Cfg(ctypes.Structure):
    _fields_ = [("sps", ctypes.c_float), ("rolloff", ctypes.c_float),
                ("rrc_delay", ctypes.c_int), ("loop_bw", ctypes.c_float),
                ("damping", ctypes.c_float), ("agc_alpha", ctypes.c_float)]


_f32p = np.ctypeslib.ndpointer(np.float32, flags="C_CONTIGUOUS")
_u64p = np.ctypeslib.ndpointer(np.uint64, flags="C_CONTIGUOUS")
_lib.drx_frontend_cfg_default.argtypes = [ctypes.POINTER(_Cfg), ctypes.c_float,
                                          ctypes.c_float]
_lib.drx_frontend_new.argtypes = [ctypes.POINTER(_Cfg)]
_lib.drx_frontend_new.restype = ctypes.c_void_p
_lib.drx_frontend_free.argtypes = [ctypes.c_void_p]
_lib.drx_frontend_process.argtypes = [ctypes.c_void_p, _f32p, ctypes.c_int, _f32p,
                                      ctypes.c_int, ctypes.POINTER(ctypes.c_int),
                                      _u64p]
_lib.drx_frontend_process.restype = ctypes.c_int
_lib.drx_frontend_gain.argtypes = [ctypes.c_void_p]
_lib.drx_frontend_gain.restype = ctypes.c_float


class Frontend:
    """Matched filter + symbol timing recovery + AGC (see frontend.h)."""

    def __init__(self, sps, rolloff, rrc_delay=10, loop_bw=1e-3, damping=1.0):
        cfg = _Cfg()
        _lib.drx_frontend_cfg_default(ctypes.byref(cfg), sps, rolloff)
        cfg.rrc_delay, cfg.loop_bw, cfg.damping = rrc_delay, loop_bw, damping
        self._fe = _lib.drx_frontend_new(ctypes.byref(cfg))
        if not self._fe:
            raise ValueError("invalid front-end configuration")
        self._strobe = np.zeros(0, np.uint64)

    def __del__(self):
        if getattr(self, "_fe", None):
            _lib.drx_frontend_free(self._fe)
            self._fe = None

    def process(self, inp, out):
        """inp, out: complex64 arrays. Returns (n_out, n_consumed, strobe)."""
        if len(self._strobe) < len(out):
            self._strobe = np.zeros(len(out), np.uint64)
        used = ctypes.c_int()
        n = _lib.drx_frontend_process(self._fe, inp.view(np.float32), len(inp),
                                      out.view(np.float32), len(out),
                                      ctypes.byref(used), self._strobe)
        return n, used.value, self._strobe[:n]

    @property
    def gain(self):
        return _lib.drx_frontend_gain(self._fe)
