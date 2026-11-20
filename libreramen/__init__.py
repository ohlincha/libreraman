"""LibreRamen: open-source software for Raman spectrometers (first supported: Anton Paar Cora 5000)."""

__version__ = "0.1.0"

from .instrument import Cora, CoraError, NotArmed  # noqa: E402
from .spectrum import Spectrum  # noqa: E402

__all__ = ["Cora", "CoraError", "NotArmed", "Spectrum", "__version__"]
