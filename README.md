# Serial_Tool_rt

## How to use

Build serial_tool_rt.c using this command

gcc -g serial_tool_rt.c -o serial_tool_rt

Once serial_tool_rt is built execute it by using this command with these arguments

./serial_tool_rt (USB Port) (Baud rate) (file with Commands to test) (file with expected return)

EXAMPLE:

./serial_tool_rt /dev/ttyUSB0 9600 test.txt expected.txt

To add more tests enter the command in test.txt and the expected result in expected.txt

