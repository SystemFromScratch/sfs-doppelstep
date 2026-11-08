# SPDX-License-Identifier: Apache-2.0
"""Host-only tests of the GUI modules (no serial port, no display)."""

import os
import sys

GUI = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, GUI)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
