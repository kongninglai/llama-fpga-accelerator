# Usage: vitis_hls -f run_hls.tcl
set here [file dirname [file normalize [info script]]]
set part xczu3eg-sbva484-1-e
set period 6.667
if {[info exists ::env(TG_PART)]} { set part $::env(TG_PART) }
if {[info exists ::env(TG_PERIOD_NS)]} { set period $::env(TG_PERIOD_NS) }
open_project -reset [file join $here build tiled_gemv]
set_top tiled_gemv_accel
add_files [file join $here tiled_gemv.cpp]
add_files -tb [file join $here tb_tiled_gemv.cpp]
open_solution -reset solution1 -flow_target vivado
set_part $part
create_clock -period $period -name default
csim_design
csynth_design
cosim_design -rtl verilog
export_design -format ip_catalog -rtl verilog
exit
