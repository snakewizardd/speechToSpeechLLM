files=$(ls testConvo*.wav)

filter_complex=""
i=0
for file in $files; do
    filter_complex+="[$i:a]"
    ((i++))
done

filter_complex+="concat=n=$i:v=0:a=1[out]"

ffmpeg $(for file in $files; do printf -- "-i %s " "$file"; done) -filter_complex "$filter_complex" -map "[out]" combined_output.wav
