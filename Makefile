picoscpi: picoscpi.c
	gcc picoscpi.c -I/opt/picoscope/include -o picoscpi -g -L/opt/picoscope/lib -lps3000a
