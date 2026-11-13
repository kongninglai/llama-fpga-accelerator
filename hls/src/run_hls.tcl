# Run: vitis_hls -f src/run_hls.tcl (or vivado_hls -f ...).
set here [file dirname [file normalize [info script]]]
open_project -reset [file join $here dotprod_hls]
set_top dotprod_accel
add_files [file join $here dotprod.cpp]
add_files -tb [file join $here tb_dotprod.cpp]
open_solution -reset solution1
# Ultra96 ZU3EG; override for a different board/part using DOTPROD_PART.
set part xczu3eg-sbva484-1-e
if {[info exists ::env(DOTPROD_PART)]} { set part $::env(DOTPROD_PART) }
set_part $part
create_clock -period 10 -name default
csim_design
csynth_design
cosim_design -rtl verilog
export_design -format ip_catalog -rtl verilog
exit
