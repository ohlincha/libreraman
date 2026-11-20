#!/usr/bin/env python3
"""Writes synthetic, truncated quantum-chemistry output excerpts for tests/golden/computed.
Numbers are realistic for CCl4, CHCl3, diamond, Si and ZnS but made up (not from real runs)."""
import math, os, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

def w(name, text):
    with open(os.path.join(out, name), "w", newline="\n") as f:
        f.write(text)

R = 1.0277   # CCl4: C-Cl 1.780 A / sqrt(3)
CCL4 = [("C", 6, (0, 0, 0)), ("Cl", 17, (R, R, R)), ("Cl", 17, (-R, -R, R)), ("Cl", 17, (-R, R, -R)), ("Cl", 17, (R, -R, -R))]
CHCL3 = [("C", 6, (0.0, 0.0, 0.4496)), ("H", 1, (0.0, 0.0, 1.5367)), ("Cl", 17, (1.6728, 0.0, -0.1475)),
         ("Cl", 17, (-0.8364, 1.4487, -0.1475)), ("Cl", 17, (-0.8364, -1.4487, -0.1475))]

# (symmetry, freq, raman activity, IR, depol P, red mass, force const)
CCL4_G = [("E", 213.6200, 4.1012, 0.0, 0.75, 34.9689, 0.9402)] * 2 + \
         [("T2", 312.4500, 3.6230, 0.3812, 0.75, 22.7718, 1.3097)] * 3 + \
         [("A1", 449.8700, 24.8310, 0.0, 0.0017, 34.9689, 4.1697)] + \
         [("T2", 770.1300, 3.0540, 320.4530, 0.75, 12.8571, 4.4931)] * 3
CHCL3_G = [("E", 259.7820, 3.1023, 0.0151, 0.75, 34.1012, 1.3559)] * 2 + \
          [("A1", 364.1540, 4.5871, 0.9421, 0.2541, 22.4301, 1.7525)] + \
          [("A1", 662.4810, 11.2034, 2.1157, 0.0612, 12.1430, 3.1401)] + \
          [("E", 737.0120, 2.4105, 176.8200, 0.75, 11.8710, 3.7991)] * 2 + \
          [("E", 1232.6450, 3.9162, 38.1120, 0.75, 1.1590, 1.0377)] * 2 + \
          [("A1", 3155.3040, 91.0412, 1.0531, 0.2903, 1.0901, 6.3946)]

def disp(nat, k):
    """deterministic pseudo normal-mode displacements"""
    v = []
    for a in range(nat):
        row = []
        for c in range(3):
            x = math.sin(1.7 * (k + 1) + 2.3 * a + 0.9 * c) * (0.6 if a == 0 else 0.35)
            row.append(round(x, 2))
        v.append(row)
    return v

def g_geom(atoms, title):
    s = f"                         {title}\n"
    s += " ---------------------------------------------------------------------\n"
    s += " Center     Atomic      Atomic             Coordinates (Angstroms)\n"
    s += " Number     Number       Type             X           Y           Z\n"
    s += " ---------------------------------------------------------------------\n"
    for i, (el, z, p) in enumerate(atoms, 1):
        s += f"{i:7d}{z:11d}{0:12d}{p[0]:16.6f}{p[1]:12.6f}{p[2]:12.6f}\n"
    s += " ---------------------------------------------------------------------\n"
    return s

FREQ_HEAD = """ Harmonic frequencies (cm**-1), IR intensities (KM/Mole), Raman scattering
 activities (A**4/AMU), depolarization ratios for plane and unpolarized
 incident light, reduced masses (AMU), force constants (mDyne/A),
 and normal coordinates:
"""

def g_normal(modes, atoms):
    s = FREQ_HEAD
    for b in range(0, len(modes), 3):
        blk = modes[b:b + 3]
        idx = range(b, b + len(blk))
        s += "                   " + "".join(f"{i + 1:4d}{'':19s}" for i in idx).rstrip() + "\n"
        s += "                   " + "".join(f"{m[0]:>4s}{'':19s}" for m in blk).rstrip() + "\n"
        def row(label, j, fmtw="11.4f"):
            return f" {label:<12s}--" + "".join(f"{format(m[j], fmtw)}{'':12s}" for m in blk).rstrip() + "\n"
        s += row("Frequencies", 1)
        s += row("Red. masses", 5)
        s += row("Frc consts", 6)
        s += row("IR Inten", 3)
        s += row("Raman Activ", 2)
        s += row("Depolar (P)", 4)
        s += f" {'Depolar (U)':<12s}--" + "".join(f"{(2 * m[4] / (1 + m[4])):11.4f}{'':12s}" for m in blk).rstrip() + "\n"
        s += ("  Atom  AN" + "".join("      X      Y      Z  " for _ in blk)).rstrip() + "\n"
        d = [disp(len(atoms), i) for i in idx]
        for a, (el, z, p) in enumerate(atoms):
            s += f"{a + 1:6d}{z:4d}" + "".join(f"  {d[q][a][0]:7.2f}{d[q][a][1]:7.2f}{d[q][a][2]:7.2f}" for q in range(len(blk))) + "\n"
    return s

def g_hp(modes, atoms):
    s = FREQ_HEAD
    for b in range(0, len(modes), 5):
        blk = modes[b:b + 5]
        idx = list(range(b, b + len(blk)))
        s += "                       " + "".join(f"{i + 1:10d}" for i in idx) + "\n"
        s += "                       " + "".join(f"{m[0]:>10s}" for m in blk) + "\n"
        for label, j in (("Frequencies", 1), ("Reduced masses", 5), ("Force constants", 6), ("IR Intensities", 3),
                         ("Raman Activities", 2)):
            s += f"{label:>19s} ---" + "".join(f"{m[j]:10.4f}" for m in blk) + "\n"
        s += "  Coord Atom Element:\n"
        d = [disp(len(atoms), i) for i in idx]
        for a, (el, z, p) in enumerate(atoms):
            for c in range(3):
                s += f"{c + 1:6d}{a + 1:5d}{z:6d}    " + "".join(f"{d[q][a][c]:10.5f}" for q in range(len(blk))) + "\n"
    return s

G_BANNER = """ Entering Gaussian System, Link 0=g16
 Input={inp}
 Output={inp_log}
 Initial command:
 /opt/g16/l1.exe "/scratch/Gau-12345.inp" -scrdir="/scratch/"
 Entering Link 1 = /opt/g16/l1.exe PID=     12346.

 Copyright (c) 1988-2019, Gaussian, Inc.  All Rights Reserved.

 ******************************************
 Gaussian 16:  ES64L-G16RevC.01  3-Jul-2019
                20-Nov-2026
 ******************************************
"""

def g_job(route, stoich, atoms, scf, modes=None, hp=False, opt=False):
    s = " %chk=job.chk\n ----------------------------------------------------------------------\n"
    s += f" {route}\n"
    s += " ----------------------------------------------------------------------\n"
    s += " 1/10=4,30=1,18=20,19=15,26=3,38=1/1,3;\n"
    s += " ---------\n synthetic\n ---------\n Symbolic Z-matrix:\n Charge =  0 Multiplicity = 1\n"
    for el, z, p in atoms:
        s += f" {el:<20s}{p[0]:10.5f}{p[1]:10.5f}{p[2]:10.5f}\n"
    s += "\n" + g_geom(atoms, "Input orientation:")
    s += " Stoichiometry    " + stoich + "\n Framework group  TD[O(C),4C3(Cl)]\n"
    s += g_geom(atoms, "Standard orientation:")
    s += " Standard basis: 6-31G(d) (6D, 7F)\n"
    s += f" SCF Done:  E(RB3LYP) =  {scf:.9f}     A.U. after   10 cycles\n"
    if opt:
        s += " Optimization completed.\n    -- Stationary point found.\n"
    if modes:
        s += "\n Full mass-weighted force constant matrix:\n"
        if hp:
            s += g_hp(modes, atoms)
        s += g_normal(modes, atoms)
        s += "\n -------------------\n - Thermochemistry -\n -------------------\n Temperature   298.150 Kelvin.  Pressure   1.00000 Atm.\n"
    s += " Normal termination of Gaussian 16 at Fri Nov 20 10:00:00 2026.\n"
    return s

# ---- Gaussian: CCl4, Link1 (opt, then freq=raman)
txt = G_BANNER.format(inp="ccl4.inp", inp_log="ccl4.log")
txt += g_job("#p b3lyp/6-31g(d) opt", "CCl4", CCL4, -1878.468123401, opt=True)
txt += " Link1:  Proceeding to internal job step number  2.\n"
txt += " Entering Link 1 = /opt/g16/l1.exe PID=     12400.\n"
txt += g_job("#p b3lyp/6-31g(d) freq=raman geom=check guess=read", "CCl4", CCL4, -1878.468123455, CCL4_G)
w("gaussian_ccl4.log", txt)

# ---- Gaussian: CHCl3, freq=(raman,hpmodes)
txt = G_BANNER.format(inp="chcl3.gjf", inp_log="chcl3.out")
txt += g_job("#p b3lyp/6-31g(d) freq=(raman,hpmodes)", "CHCl3", CHCL3, -1419.102345678, CHCL3_G, hp=True)
w("gaussian_chcl3_hpmodes.out", txt)

# ---- ORCA
def orca_banner(ver):
    return f"""
                                 *****************
                                 * O   R   C   A *
                                 *****************

           --- An Ab Initio, DFT and Semiempirical electronic structure package ---

                  #######################################################
                  #                        -***-                        #
                  #          Department of theory and spectroscopy      #
                  #######################################################


                         Program Version {ver} -  RELEASE  -

"""

def orca_input(lines):
    s = "================================================================================\n"
    s += "                                       INPUT FILE\n"
    s += "================================================================================\n"
    s += "NAME = mol.inp\n"
    for i, l in enumerate(lines, 1):
        s += f"|{i:3d}> {l}\n"
    s += f"|{len(lines) + 1:3d}>                          ****END OF INPUT****\n"
    s += "================================================================================\n\n"
    return s

def orca_coords(atoms):
    s = "---------------------------------\nCARTESIAN COORDINATES (ANGSTROEM)\n---------------------------------\n"
    for el, z, p in atoms:
        s += f"  {el:<4s}{p[0]:12.6f}{p[1]:12.6f}{p[2]:12.6f}\n"
    s += "\n----------------------------\nCARTESIAN COORDINATES (A.U.)\n----------------------------\n"
    s += "  NO LB      ZA    FRAG     MASS         X           Y           Z\n"
    for i, (el, z, p) in enumerate(atoms):
        s += f"   {i} {el:<3s}{z:8.4f}    0    12.011{p[0] / 0.529177:12.6f}{p[1] / 0.529177:12.6f}{p[2] / 0.529177:12.6f}\n"
    return s + "\n"

def orca_freq(modes, irstyle, imag=()):
    nat3 = 6 + len(modes)
    s = "-----------------------\nVIBRATIONAL FREQUENCIES\n-----------------------\n\n"
    s += "Scaling factor for frequencies =  1.000000000  (already applied!)\n\n"
    for i in range(6):
        s += f"   {i:2d}:         0.00 cm**-1\n"
    for i, m in enumerate(modes, 6):
        s += f"   {i:2d}:     {m[1]:8.2f} cm**-1" + (" ***imaginary mode***" if m[1] < 0 else "") + "\n"
    s += "\n\n------------\nNORMAL MODES\n------------\n\nThese modes are the Cartesian displacements weighted by the diagonal matrix\n"
    s += "M(i,i)=1/sqrt(m[i]) where m[i] is the mass of the displaced atom\nThus, these vectors are normalized but *not* orthogonal\n\n"
    s += "                  0          1          2          3          4          5    \n"
    s += "      0       0.000000   0.000000   0.000000   0.000000   0.000000   0.000000\n\n"
    s += "-----------\nIR SPECTRUM\n-----------\n\n"
    if irstyle == 4:
        s += " Mode    freq (cm**-1)   T**2         TX         TY         TZ\n"
        s += "-------------------------------------------------------------------\n"
        for i, m in enumerate(modes, 6):
            s += f"  {i:2d}:   {m[1]:10.2f}   {m[3]:10.6f}  ({0.0:10.6f} {0.0:10.6f} {0.0:10.6f})\n"
    else:
        s += " Mode   freq       eps      Int      T**2         TX        TY        TZ\n"
        s += "       cm**-1   L/(mol*cm) km/mol    a.u.\n"
        s += "----------------------------------------------------------------------------\n"
        for i, m in enumerate(modes, 6):
            if m[3] == 0:
                continue          # ORCA lists only modes with intensity > 0
            s += f"  {i:2d}:   {m[1]:7.2f}   {m[3] / 5000:8.6f}  {m[3]:7.2f}  {m[3] / 4000:8.6f}  ({0.01:9.6f} {0.0:9.6f} {0.0:9.6f})\n"
    s += "\n* The epsilon (eps) is given for a Dirac delta lineshape.\n\n"
    s += "--------------\nRAMAN SPECTRUM\n--------------\n\n"
    s += " Mode    freq (cm**-1)   Activity   Depolarization\n"
    s += "-------------------------------------------------------------------\n"
    for i, m in enumerate(modes, 6):
        s += f"  {i:2d}:   {m[1]:10.2f}   {m[2]:11.6f}   {m[4]:11.6f}\n"
    s += "\nThe first frequency considered to be a vibration is 6\nThe total number of vibrations considered is " + str(len(modes)) + "\n\n"
    return s

def scaled(modes, f):
    return [(m[0], round(m[1] * f, 2)) + m[2:] for m in modes]

txt = orca_banner("5.0.4") + orca_input(["! B3LYP def2-TZVP TightSCF Freq", "%elprop Polar 1 end", "* xyzfile 0 1 chcl3.xyz"])
txt += orca_coords(CHCL3) + "Your calculation utilizes the basis: def2-TZVP\n\n"
txt += "Total Energy       :        -1419.53212345 Eh\n\n"
txt += orca_freq(scaled(CHCL3_G, 1.004), 5)
txt += "                             ****ORCA TERMINATED NORMALLY****\n"
w("orca5_chcl3.out", txt)

txt = orca_banner("4.2.1") + orca_input(["! BP86 def2-SVP Freq", "%elprop Polar 1 end", "* xyz 0 1", "C 0 0 0", "*",
                                         "$new_job", "! B3LYP def2-TZVP Freq", "%elprop Polar 1 end", "* xyz 0 1", "*"])
txt += orca_coords(CCL4) + "Your calculation utilizes the basis: def2-SVP\n\n"
txt += orca_freq(scaled(CCL4_G, 0.955), 4)
txt += "\n                         $$$$$$$$$$$$$$$$  JOB NUMBER  2 $$$$$$$$$$$$$$\n\n"
txt += orca_coords(CCL4) + "Your calculation utilizes the basis: def2-TZVP\n\n"
txt += orca_freq(scaled(CCL4_G, 1.013), 4)
txt += "                             ****ORCA TERMINATED NORMALLY****\n"
w("orca4_ccl4_twojobs.out", txt)

txt = orca_banner("6.0.1") + orca_input(["! r2SCAN-3c Freq", "%elprop Polar 1 end", "* xyzfile 0 1 ccl4.xyz"])
txt += orca_coords(CCL4) + "Your calculation utilizes the basis: def2-mTZVPP\n\n"
txt += orca_freq(scaled(CCL4_G, 1.022), 6)
txt += "                             ****ORCA TERMINATED NORMALLY****\n"
w("orca6_ccl4.out", txt)

# ORCA .hess (CHCl3)
mods = scaled(CHCL3_G, 1.004)
n = 15
s = "\n$orca_hessian_file\n\n$act_atom\n  0\n\n$act_coord\n  0\n\n$act_energy\n        0.000000\n\n$hessian\n15\n"
s += "                  0          1          2          3          4          5\n"
s += "      0       0.512345   0.000000   0.000000  -0.123456   0.000000   0.000000\n"
s += "\n$vibrational_frequencies\n15\n"
for i in range(6):
    s += f"    {i:2d}        0.000000\n"
for i, m in enumerate(mods, 6):
    s += f"    {i:2d}     {m[1]:11.6f}\n"
s += "\n$normal_modes\n15 15\n                  0          1          2          3          4          5\n"
s += "      0       0.000000   0.000000   0.000000   0.000000   0.000000   0.000000\n"
s += "\n#\n# The atoms: label  mass x y z (in bohrs)\n#\n$atoms\n5\n"
for el, z, p in CHCL3:
    mass = {"C": 12.011, "H": 1.008, "Cl": 35.453}[el]
    s += f" {el:<3s}{mass:10.4f}  {p[0] / 0.529177210903:12.6f} {p[1] / 0.529177210903:12.6f} {p[2] / 0.529177210903:12.6f}\n"
s += "\n$actual_temperature\n  0.000000\n\n$ir_spectrum\n15\n"
for i in range(6):
    s += "      0.00       0.00000000       0.00000000       0.00000000       0.00000000       0.00000000       0.00000000\n"
for m in mods:
    s += f"   {m[1]:7.2f}       {m[3] / 5000:.8f}     {m[3]:12.8f}       {m[3] / 4000:.8f}       0.01000000       0.00000000       0.00000000\n"
s += "\n$raman_spectrum\n15\n"
for i in range(6):
    s += "     0.000000     0.000000     0.000000\n"
for m in mods:
    s += f"  {m[1]:11.6f}  {m[2]:11.6f}  {m[4]:11.6f}\n"
s += "\n$end\n"
w("orca_chcl3.hess", s)

# ---- NWChem (CCl4)
mods = scaled(CCL4_G, 1.0004)
s = """ argument  1 = ccl4.nw



============================== echo of input deck ==============================
start ccl4
geometry
 C 0 0 0
 ...
end
basis
 * library 6-31G*
end
dft
 xc b3lyp
end
task dft raman
================================================================================






              Northwest Computational Chemistry Package (NWChem) 7.0.2
              --------------------------------------------------------


                    Environmental Molecular Sciences Laboratory
                       Pacific Northwest National Laboratory
                                Richland, WA 99352

                             Geometry "geometry" -> ""
                             -------------------------

 Output coordinates in angstroms (scale by  1.889725989 to convert to a.u.)

  No.       Tag          Charge          X              Y              Z
 ---- ---------------- ---------- -------------- -------------- --------------
"""
for i, (el, z, p) in enumerate(CCL4, 1):
    s += f" {i:4d} {el:<16s}{z:11.4f}{p[0]:15.8f}{p[1]:15.8f}{p[2]:15.8f}\n"
s += """
      Atomic Mass
      -----------

      C                 12.000000
      Cl                34.968850


                      Basis "ao basis" -> "ao basis" (cartesian)
                      -----
 Summary of "ao basis" -> "ao basis" (cartesian)
 ------------------------------------------------------------------------------
       Tag                 Description            Shells   Functions and Types
 ---------------- ------------------------------  ------  ---------------------
 C                           6-31G*                  10       15   4s3p1d
 Cl                          6-31G*                  14       23   5s4p1d


                                 NWChem DFT Module
                                 -----------------

              XC Information
              --------------
                         B3LYP Method XC Potential
                     Hartree-Fock (Exact) Exchange  0.200
                        Slater Exchange Functional  0.800 local

         Total DFT energy =    -1878.468001234567

"""
def nw_table(title, units, dashes, rows):
    t = " ----------------------------------------------------------------------------\n"
    t += f" Normal Eigenvalue ||{title}\n{units}\n{dashes}\n"
    for r in rows:
        t += r + "\n"
    t += " ----------------------------------------------------------------------------\n"
    return t
irrows = [f" {i:4d} {0.0:12.3f} || {0.0:14.6f} {0.0:17.3f} {0.0:13.3f} {0.0:11.3f}" for i in range(1, 7)]
irrows += [f" {i:4d} {m[1]:12.3f} || {m[3] / 974.88:14.6f} {m[3] / 42.2561:17.3f} {m[3]:13.3f} {m[3] / 3.2:11.3f}" for i, m in enumerate(mods, 7)]
s += nw_table("           Infra Red Intensities", "  Mode   [cm**-1]  || [atomic units] [(debye/angs)**2] [(KM/mol)] [arbitrary]",
              " ------ ---------- || -------------- ----------------- ---------- -----------", irrows)
s += "\n\n"
s += nw_table("           Projected Infra Red Intensities", "  Mode   [cm**-1]  || [atomic units] [(debye/angs)**2] [(KM/mol)] [arbitrary]",
              " ------ ---------- || -------------- ----------------- ---------- -----------", irrows)
s += "\n\n ==================== Raman (static polarizability derivatives) ====================\n\n"
rr = []
for i, m in enumerate(mods, 7):
    # S = 45 a^2 + 7 g^2 ; depol = 3 g^2 / (45 a^2 + 4 g^2)
    S, d = m[2], m[4]
    g2 = 45 * d * S / (3 * 45 + 4 * 45 * d - 7 * 45 * d) if d < 0.74 else S / 7
    a2 = (S - 7 * g2) / 45
    rr.append(f" {i:4d} {m[1]:12.3f} || {a2:12.6f} {g2:12.6f} {1 - d:12.6f} {d:12.6f} {S:12.6f}")
s += nw_table("                 Raman Scattering Activities",
              "  Mode   [cm**-1]  ||  [alpha^2]    [beta^2]    [Pol Ratio]  [Depol Ratio]  [ Activity ]",
              " ------ ---------- || ------------ ------------ ------------ ------------ ------------", rr)
s += "\n Task  times  cpu:      312.4s     wall:      320.1s\n"
w("nwchem_ccl4_raman.out", s)

# ---- CP2K (CHCl3, IR only, one imaginary mode)
s = """ DBCSR| CPU Multiplication driver                                           XSMM

  **** **** ******  **  PROGRAM STARTED AT               2026-11-20 10:00:00.000
 ***** ** ***  *** **   PROGRAM STARTED ON                              node01
 **    ****   ******    PROGRAM STARTED BY                                user
 ***** **    ** ** **   PROGRAM PROCESS ID                               12345
  **** **  *******  **  PROGRAM STARTED IN /scratch/chcl3

 CP2K| version string:                                          CP2K version 2023.1
 CP2K| source code revision number:                                  git:4e1cd1d
 GLOBAL| Run type                                                 VIBRATIONAL_ANALYSIS

 FUNCTIONAL| ROUTINE=NEW
 FUNCTIONAL| PBE:
 FUNCTIONAL| J.P. Perdew, K. Burke, M. Ernzerhof, Phys. Rev. Letter, vol. 77, n 18,
 FUNCTIONAL| pp. 3865-3868, (1996){spin unpolarized}

 ATOMIC KIND INFORMATION

  1. Atomic kind: C                                     Number of atoms:       1

     Orbital Basis Set                                             DZVP-MOLOPT-SR-GTH

 MODULE QUICKSTEP:  ATOMIC COORDINATES IN angstrom
  Atom  Kind  Element       X           Y           Z          Z(eff)       Mass

"""
for i, (el, z, p) in enumerate(CHCL3, 1):
    kind = {"C": 1, "H": 2, "Cl": 3}[el]
    zeff = {"C": 4.0, "H": 1.0, "Cl": 7.0}[el]
    mass = {"C": 12.0107, "H": 1.0079, "Cl": 35.4530}[el]
    s += f"{i:7d}{kind:6d} {el:<2s}{z:5d}{p[0]:12.6f}{p[1]:12.6f}{p[2]:12.6f}{zeff:12.2f}{mass:13.4f}\n"
s += "\n\n VIB| Vibrational Analysis Info\n VIB|                         NORMAL MODES - CARTESIAN DISPLACEMENTS\n VIB|\n"
cp = [-25.3124, 252.1043, 252.1043, 355.9211, 650.3108, 718.4402, 718.4402, 1196.2017, 3087.5531]
cir = [0.00012, 0.01452, 0.01452, 0.88104, 1.90233, 170.11290, 170.11290, 35.72011, 0.98761]
for b in range(0, 9, 3):
    idx = range(b, b + 3)
    s += " VIB|" + "".join(f"{i + 1:23d}" for i in idx) + "\n"
    s += " VIB|Frequency (cm^-1)    " + "".join(f"{cp[i]:23.6f}" for i in idx) + "\n"
    s += " VIB|IR int (KM/Mole)     " + "".join(f"{cir[i]:23.6f}" for i in idx) + "\n"
    s += " VIB|Red.Masses (a.u.)    " + "".join(f"{12.5:23.6f}" for i in idx) + "\n"
    s += " VIB|Frc consts (a.u.)    " + "".join(f"{0.0123:23.6f}" for i in idx) + "\n\n"
    s += "  ATOM  EL  " + "      X      Y      Z  " * 3 + "\n"
    for a, (el, z, p) in enumerate(CHCL3, 1):
        s += f"  {a:4d}  {el:<3s}" + "".join(f"  {0.1:7.3f}{-0.2:7.3f}{0.05:7.3f}" for _ in idx) + "\n"
    s += "\n"
s += " VIB| NORMAL MODES - CARTESIAN DISPLACEMENTS\n\n  **** **** ******  **  PROGRAM ENDED AT                 2026-11-20 11:00:00.000\n"
w("cp2k_chcl3_vib.out", s)

# ---- Quantum ESPRESSO (diamond): dynmat.x output and ph.x output
s = """
     Program DYNMAT v.7.2 starts on 20Nov2026 at 10:12: 1

     This program is part of the open-source Quantum ESPRESSO suite
     for quantum simulation of materials; please cite
         "P. Giannozzi et al., J. Phys.:Condens. Matter 21 395502 (2009);
          URL http://www.quantum-espresso.org",

     Parallel version (MPI), running on     1 processors

     Reading Dynamical Matrix from file dyn
     ...Force constants read
     ...epsilon and Z* read
     ...Raman cross sections read
     Acoustic Sum Rule: || Z(ASR) - Z || =  0.000000E+00
     Acoustic Sum Rule: || dyn(ASR) - dyn || =  3.461210E-03
     A direction for q was not specified:TO-LO splitting will be absent

     Polarizability (A^3 units)
     multiply by 0.954372 for Clausius-Mossotti correction
         6.284171    0.000000    0.000000
         0.000000    6.284171    0.000000
         0.000000    0.000000    6.284171

     IR activities are in (D/A)^2/amu units
     Raman activities are in A^4/amu units
     multiply Raman by 0.910826 for Clausius-Mossotti correction

# mode   [cm-1]    [THz]      IR          Raman   depol.fact
    1      0.00    0.0000    0.0000         0.0000    0.0000
    2      0.00    0.0000    0.0000         0.0000    0.0000
    3      0.00    0.0000    0.0000         0.0000    0.0000
    4   1293.21   38.7695    0.0000        84.3412    0.7500
    5   1293.21   38.7695    0.0000        84.3412    0.7500
    6   1293.21   38.7695    0.0000        84.3412    0.7500

     DYNMAT       :      0.01s CPU      0.02s WALL

"""
w("qe_diamond_dynmat.out", s)

s = """
     Program PHONON v.7.2 starts on 20Nov2026 at 10:00: 0

     This program is part of the open-source Quantum ESPRESSO suite

     bravais-lattice index     =            2
     lattice parameter (alat)  =       6.7400  a.u.
     Exchange-correlation= PBE
                           (   1   4   3   4   0   0   0)

     celldm(1)=    6.74000  celldm(2)=    0.00000  celldm(3)=    0.00000

     site n.  atom      mass           positions (alat units)
        1     C   12.0107   tau(    1) = (    0.00000    0.00000    0.00000  )
        2     C   12.0107   tau(    2) = (    0.25000    0.25000    0.25000  )

     Computing dynamical matrix for
                    q = (   0.0000000   0.0000000   0.0000000 )

     Diagonalizing the dynamical matrix

     q = (    0.000000000   0.000000000   0.000000000 )

 **************************************************************************
     freq (    1) =      -0.040562 [THz] =      -1.352989 [cm-1]
     freq (    2) =      -0.040562 [THz] =      -1.352989 [cm-1]
     freq (    3) =      -0.040562 [THz] =      -1.352989 [cm-1]
     freq (    4) =      38.769549 [THz] =    1293.212342 [cm-1]
     freq (    5) =      38.769549 [THz] =    1293.212342 [cm-1]
     freq (    6) =      38.769549 [THz] =    1293.212342 [cm-1]
 **************************************************************************

     Mode symmetry, O_h (m-3m)  point group:

     freq (  1 -  3) =         -1.4  [cm-1]   --> T_1u G_15  G_4- I
     freq (  4 -  6) =       1293.2  [cm-1]   --> T_2g G_25' G_5+ R

     PHONON       :     12.31s CPU     13.02s WALL

"""
w("qe_diamond_ph.out", s)

# ---- vasp_raman.py (CCl4 in a box, PBE; modes in OUTCAR order, descending)
mods = scaled(CCL4_G, 0.982)
rows = [(m[1], m[2]) for m in reversed(mods)] + [(18.1, 1.2e-5), (12.4, 3.0e-6), (9.3, 0.0), (4.2, 0.0), (-6.1, 1.0e-6), (-11.7, 2.0e-6)]
s = "# mode    freq(cm-1)    alpha    beta2    activity\n"
for i, (f, a) in enumerate(rows, 1):
    alpha = math.sqrt(a / 45 / 2)
    beta2 = (a - 45 * alpha * alpha) / 7
    s += "%03d  %10.5f  %10.7f  %10.7f  %10.7f\n" % (i, f, alpha, beta2, a)
w("vasp_raman_ccl4.dat", s)

# ---- Phonopy-Spectroscopy peak table (ZnS zinc blende, TO mode at Gamma, no NAC)
s = "Peak   Frequency [inv. cm]   Irrep.   Intensity [Å^4 amu^-1]   Linewidth [inv. cm]\n"
s += "   1                0.0000       T2                   0.0000                0.0000\n"
s += "   2              271.4120       T2                  15.6213                4.1200\n"
w("phonopy_zns_raman_peaktable.dat", s)

# ---- CASTEP (Si, Gamma point, IR + Raman)
s = """
 +-------------------------------------------------+
 |                                                 |
 |      CCC   AA    SSS  TTTTT  EEEEE  PPPP        |
 |     C     A  A  S       T    E      P   P       |
 |     C     AAAA   SS     T    EEE    PPPP        |
 |     C     A  A     S    T    E      P           |
 |      CCC  A  A  SSS     T    EEEEE  P           |
 |                                                 |
 +-------------------------------------------------+
 |                                                 |
 | Welcome to Academic Release CASTEP version 20.11|
 | Ab Initio Total Energy Program                  |
 +-------------------------------------------------+

 ************************************ Title ************************************
 Si phonon + Raman

 ***************************** General Parameters ******************************
 type of calculation                            : phonon calculation

 *********************** Exchange-Correlation Parameters ***********************
 using functional                               : Local Density Approximation

 ************************* Basis Set Parameters *************************
 plane wave basis set cut-off                   :   400.0000   eV

                           -------------------------------
                                      Cell Contents
                           -------------------------------

            xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
            x  Element    Atom        Fractional coordinates of atoms  x
            x            Number           u          v          w      x
            x----------------------------------------------------------x
            x  Si           1         0.000000   0.000000   0.000000   x
            x  Si           2         0.250000   0.250000   0.250000   x
            xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

 ===============================================================================
 +                           Vibrational Frequencies                           +
 +                           -----------------------                           +
 +                                                                             +
 + Performing frequency calculation at  1 wavevector  (q-pt )                  +
 + ==========================================================================  +
 +                                                                             +
 +   q-pt=    1 (  0.000000  0.000000  0.000000)     1.0000000000              +
 +                                                                             +
 + ------------------------------------------------------------------------------- +
 +  N      Frequency irrep.    IR intensity active            Raman activity active +
 +                (cm-1)         ((D/A)**2/amu)                  (A**4 amu**(-1))   +
 +                                                                                 +
 +  1      -0.031612   a          0.0000000  N                  0.0000000  N        +
 +  2      -0.031612   a          0.0000000  N                  0.0000000  N        +
 +  3      -0.031612   a          0.0000000  N                  0.0000000  N        +
 +  4     508.456281   b          0.0000000  N                 31.2341213  Y        +
 +  5     508.456281   b          0.0000000  N                 31.2341213  Y        +
 +  6     508.456281   b          0.0000000  N                 31.2341213  Y        +
 + .................................................................................. +
 +        Character table from group theory analysis of eigenvectors               +
 +                           Point Group =  32, Oh                                 +
 + .................................................................................. +
 +  Rep  Mul |    E   C3   C2   C4  C2'    I  S6   s_h  S4  s_d                    +
 +  --- --- + ---------------------------------------------------                   +
 + a T1u  1 |    3    0   -1    1   -1   -3    0    1   -1    1                    +
 + b T2g  1 |    3    0   -1   -1    1    3    0   -1   -1    1                    +
 ===============================================================================

 Total time          =         45.21 s
"""
w("castep_si_raman.castep", s)

# ---- not a quantum-chemistry output (for the error message test)
w("not_an_output.txt", "This is just a note.\n1 2\n3 4\n")
# ---- Gaussian without freq job (no frequencies)
w("gaussian_opt_only.log", G_BANNER.format(inp="x.inp", inp_log="x.log") + g_job("#p b3lyp/6-31g(d) opt", "CCl4", CCL4, -1878.4681, opt=True))
print("ok")
