function get_MSR_COS() {
    run_core=$1
    msr_cos_base=0xC90
    cos=$(( ${msr_cos_base} + ${run_core} ))
    run_msr_cos=0x`echo "obase=16 ;$cos"|bc`
    echo "${run_msr_cos}"
}
#echo $(($(get_MSR_COS 1)==0xC91)) #1
function set_COS_All() {
           
    strCPS="$@"
    #echo $strCPS
    declare -a CPS
    IFS=' ' read -ra CPS <<<$strCPS
    NR_CPUS=8
    MSR_COS_BASE=0xC90
    for ((i=0;i<${NR_CPUS}; i+=1)); do
        echo -n "[P${i}] COS="
        COSi=$((${MSR_COS_BASE} + ${i}))
        MSR_COSi=0x`echo "obase=16 ;$COSi"|bc`
        rdmsr -p ${i} ${MSR_COSi}
        echo "rdmsr -p ${i} ${MSR_COSi}"
        echo -n "     --> "
        printf "%08x\n" ${CPS}
        echo "wrmsr -p ${i} 0x${MSR_COSi} ${CPS[${i}]}"
        sudo wrmsr -p ${i} ${MSR_COSi} ${CPS[${i}]}
    done
}

#set_COS_All 0x1 0x1 0x1 0x1 0x1 0x1 0x1 0x1
modprobe msr
set_COS_All 0x3 0x3 0x3 0x3 0x3 0x3 0x3 0x3
