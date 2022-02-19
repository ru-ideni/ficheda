#   File Check Daemon Test
#
#   Общий алгоритм тестирования
#   -   остановка демона, если запущен
#   -   создание и заполнение тестового каталога с фалами
#   -   запуск демона
#   -   проверка первого JSON-файла на OK
#   -   создание нового файла (из первого оригинального)
#   -   проверка JSON-файла на пункт ADD
#   -   удалние первого оригинального файла
#   -   проверка JSON-файла на пункт DELETED
#   -   изменение второго оригинального
#   -   проверка JSON-файла на пункт FAIL
#   -   восстановление всех файлов в начальное состояние
#   -   проверка последнего JSON-файла на OK
#   -   остановка демона
#   -   завершение работы
#

import os
import random
import time
import string
import zlib
import json
import datetime

mission_dir = "/tmp/ficheda/"
mission_json = "/tmp/ficheda.json"
mission_interval = 33
mission_cmd = f"./bin/ficheda -p {mission_dir} -i {mission_interval} -j {mission_json}"
json_file_mtime = 0.0
num_ori_files = 5
wait_a_few_seconds = 5


def is_ficheda_running():
    data = [(int(p), c) for p, c in [x.rstrip("\n").split(" ", 1) for x in os.popen("ps h -eo pid:1,command")]]
    for proc in data:
        if str(proc[1] + " ").find("/ficheda ") > -1:
            return True
    return False


def ficheda_failure(failure_message):
    print("Failure! " + failure_message)
    print("\nkillall -TERM ficheda")
    os.popen("killall -TERM ficheda")
    print("\nBay!")
    exit(1)


def ficheda_must_be():
    if not is_ficheda_running():
        ficheda_failure("Daemon 'ficheda' must be!")


def crc32(file_name_for_crc32):
    with open(file_name_for_crc32, "rb") as fh:
        ih = 0
        while True:
            s = fh.read(65536)
            if not s:
                break
            ih = zlib.crc32(s, ih)
        return "%08X" % (ih & 0xFFFFFFFF)


def ls_mission_dir():
    time.sleep(wait_a_few_seconds)
    ls_cmd = f"ls -lp {mission_dir}"
    print("\n" + ls_cmd)
    ls_out = os.popen(ls_cmd)
    for ls_str in ls_out:
        print(">> " + ls_str.strip())


def read_next_json():
    global json_file_mtime
    ficheda_must_be()
    print("\nWait for new mission json file...")
    while True:
        ts = datetime.datetime.now().timestamp()
        try:
            if json_file_mtime != os.path.getmtime(mission_json):
                json_file = open("/tmp/ficheda.json")
                break
            else:
                print(f"[{ts}] Old json file, wait next...", end="\r")
                time.sleep(wait_a_few_seconds)
        except IOError as current_error:
            print(f"[{ts}] {current_error}", end="\r")
            time.sleep(wait_a_few_seconds)
    print("")
    new_json_data = json.load(json_file)
    json_file.close()
    # print(json.dumps(json_data, sort_keys=False, indent=2, separators=(",", ": ")))
    json_file_mtime = os.path.getmtime(mission_json)
    # print(new_json_data)
    return new_json_data


def json_array_to_dict(new_json_array):
    # convert json_array to json_dict
    new_json_dict = {}
    for ii in range(len(new_json_array)):
        path = new_json_array[ii]["path"]
        status = new_json_array[ii]["status"]
        if "etalon_crc32" in new_json_array[ii]:
            etalon_crc32 = new_json_array[ii]["etalon_crc32"]
        else:
            etalon_crc32 = None
        if "result_crc32" in new_json_array[ii]:
            result_crc32 = new_json_array[ii]["result_crc32"]
        else:
            result_crc32 = None
        new_json_dict[path] = [status, etalon_crc32, result_crc32]
    return new_json_dict


def the_first_file_must_be_perfect():
    global files_dict
    # load JSON-file
    json_array = read_next_json()
    json_dict = json_array_to_dict(json_array)
    # print("\n" + str(json_array))
    # print("\n" + str(json_dict))
    # print(json.dumps(json_dict, sort_keys=True, indent=2, separators=(",", ": ")))
    # The first file must be perfect!
    fdkl = len(files_dict.keys())
    jdkl = len(json_dict.keys())
    # compare lengths
    if fdkl != jdkl:
        print("\nThe first JSON-file is not perfect!")
        print(f"Number of original files {fdkl}, but JSON-file contain {jdkl} entries.")
        exit(1)
    # check file datas
    for file in files_dict.keys():
        if file in json_dict.keys():
            if json_dict[file][0] != "OK" or json_dict[file][1] != json_dict[file][2] or json_dict[file][1] != files_dict[file]:
                print("\nThe first JSON-file is not perfect!")
                print(f"Wrong file data: status='{json_dict[file][0]}', original_crc32={json_dict[file][1]}, result_crc32={json_dict[file][2]}")
                exit(1)
        else:
            print("\nThe first JSON-file is not perfect!")
            print(f"File {file} not exists in JSON-file.")
            exit(1)
    print("\nThe first JSON-file is perfect!!!")


def the_last_file_must_be_perfect():
    global files_dict
    # load JSON-file
    json_array = read_next_json()
    json_dict = json_array_to_dict(json_array)
    # print("\n" + str(json_array))
    # print("\n" + str(json_dict))
    # print(json.dumps(json_dict, sort_keys=True, indent=2, separators=(",", ": ")))
    # The first file must be perfect!
    fdkl = len(files_dict.keys())
    jdkl = len(json_dict.keys())
    # compare lengths
    if fdkl != jdkl:
        print("\nThe last JSON-file is not perfect!")
        print(f"Number of original files {fdkl}, but JSON-file contain {jdkl} entries.")
        exit(1)
    # check file datas
    for file in files_dict.keys():
        if file in json_dict.keys():
            if json_dict[file][0] != "OK" or json_dict[file][1] != json_dict[file][2] or json_dict[file][1] != files_dict[file]:
                print("\nThe last JSON-file is not perfect!")
                print(f"Wrong file data: status='{json_dict[file][0]}', original_crc32={json_dict[file][1]}, result_crc32={json_dict[file][2]}")
                exit(1)
        else:
            print("\nThe last JSON-file is not perfect!")
            print(f"File {file} not exists in JSON-file.")
            exit(1)
    print("\nThe last JSON-file is perfect!!!")


def ficheda_sigusr1():
    ficheda_must_be()
    print("\nSignal USR1 to daemom...")
    os.popen("killall -USR1 ficheda")


def ficheda_sigterm():
    print("\nkillall -TERM ficheda")
    os.popen("killall -TERM ficheda")
    print("\nBay!")


def create_fill_new_file(fname):
    try:
        new_fout = open(fname, "+w")
        rndstr = "".join(random.choices(string.ascii_uppercase + string.digits + string.ascii_lowercase,
                                        k=random.randint(1000, 9999)))
        for i1 in range(random.randint(1000, 9999)):
            new_fout.write(rndstr)
        new_fout.close()
    except IOError as io_error:
        print("\nSorry...")
        print(io_error)
        exit(0)


def test_new_file_mode():
    global new_file_name1, del_file_name
    # test NEW file mode
    print("\nNow... add new file...")
    # create new file
    new_file_name1 = mission_dir + "file_" + str(num_ori_files+1).zfill(4) + ".data"
    del_file_name = list(files_dict.keys())[0]
    new_cmd = f"cp -v {del_file_name} {new_file_name1}"
    print(new_cmd)
    new_out = os.popen(new_cmd)
    for new_str in new_out:
        print(">> " + new_str.strip())
    # list directory
    ls_mission_dir()
    # ficheda_sigusr1()
    tnfm_json_array = read_next_json()
    tnfm_json_dict = json_array_to_dict(tnfm_json_array)
    # print("\n" + str(tnfm_json_array))
    # print("\n" + str(tnfm_json_dict))
    # check tnfm_json_dict
    if new_file_name1 in tnfm_json_dict.keys():
        print("\n" + f"Success! File '{new_file_name1}' exists in JSON-file.")
        if tnfm_json_dict[new_file_name1][0] == "NEW":
            print(f"Success! File 'status' is 'NEW'.")
            if tnfm_json_dict[new_file_name1][1] is None:
                print(f"Success! File 'etalon_crc32' is 'None'.")
                if tnfm_json_dict[new_file_name1][2] is None:
                    print(f"Success! File 'result_crc32' is 'None'.")
                else:
                    ficheda_failure(f"File 'result_crc32' is not 'None'.")
            else:
                ficheda_failure(f"File 'etalon_crc32' is not 'None'.")
        else:
            ficheda_failure(f"File 'status' is not 'NEW'")
    else:
        ficheda_failure(f"File '{new_file_name1}' not exists in JSON-file!")


def test_del_file_mode():
    global del_file_name
    # test NEW file mode
    print("\nNow... delete file...")
    # delete file
    del_cmd = f"rm -v {del_file_name}"
    print(del_cmd)
    del_out = os.popen(del_cmd)
    for del_str in del_out:
        print(">> " + del_str.strip())
    # list directory
    ls_mission_dir()
    # ficheda_sigusr1()
    tnfm_json_array = read_next_json()
    tnfm_json_dict = json_array_to_dict(tnfm_json_array)
    # print("\n" + str(tnfm_json_array))
    # print("\n" + str(tnfm_json_dict))
    # check tnfm_json_dict
    if del_file_name in tnfm_json_dict.keys():
        print("\n" + f"Success! File '{del_file_name}' exists in JSON-file.")
        if tnfm_json_dict[del_file_name][0] == "DELETED":
            print(f"Success! File 'status' is 'DELETED'.")
            if tnfm_json_dict[del_file_name][1] is None:
                print(f"Success! File 'etalon_crc32' is 'None'.")
                if tnfm_json_dict[del_file_name][2] is None:
                    print(f"Success! File 'result_crc32' is 'None'.")
                else:
                    ficheda_failure(f"File 'result_crc32' is not 'None'.")
            else:
                ficheda_failure(f"File 'etalon_crc32' is not 'None'.")
        else:
            ficheda_failure(f"File 'status' is not 'DELETED'")
    else:
        ficheda_failure(f"File '{del_file_name}' not exists in JSON-file!")


def test_chn_file_mode():
    global new_file_name2, chn_file_name
    # test NEW file mode
    print("\nNow... change some file...")
    # select file
    chn_file_name = new_file_name1
    for key in files_dict.keys():
        chn_file_name = key
        if chn_file_name != new_file_name1 and chn_file_name != del_file_name:
            break
    if chn_file_name == new_file_name1 or chn_file_name == del_file_name:
        print("\nSorry...")
        print("Mission directory not have some files to change.")
        ficheda_sigterm()
    # store file to new
    new_file_name2 = mission_dir + "file_" + str(num_ori_files+2).zfill(4) + ".data"
    new_cmd = f"cp -v {chn_file_name} {new_file_name2}"
    print(new_cmd)
    new_out = os.popen(new_cmd)
    for new_str in new_out:
        print(">> " + new_str.strip())
    # recreate file
    create_fill_new_file(chn_file_name)
    chn_crc32 = "0x"+crc32(chn_file_name)
    print(" " + file_name + ": " + chn_crc32)
    # list directory
    ls_mission_dir()
    # ficheda_sigusr1()
    tnfm_json_array = read_next_json()
    tnfm_json_dict = json_array_to_dict(tnfm_json_array)
    # print("\n" + str(tnfm_json_array))
    # print("\n" + str(tnfm_json_dict))
    # check tnfm_json_dict
    if new_file_name1 in tnfm_json_dict.keys():
        print("\n" + f"Success! File '{chn_file_name}' exists in JSON-file.")
        if tnfm_json_dict[chn_file_name][0] == "FAIL":
            print(f"Success! File 'status' is 'FAIL'.")
            if tnfm_json_dict[chn_file_name][1] == files_dict[chn_file_name]:
                print(f"Success! File 'etalon_crc32' == '{files_dict[chn_file_name]}'.")
                if tnfm_json_dict[chn_file_name][2] == chn_crc32:
                    print(f"Success! File 'result_crc32' == '{chn_crc32}'.")
                else:
                    ficheda_failure(f"File 'result_crc32' != '{chn_crc32}'.")
            else:
                ficheda_failure(f"File 'etalon_crc32' != '{files_dict[chn_file_name]}'.")
        else:
            ficheda_failure(f"File 'status' is not 'FAIL'")
    else:
        ficheda_failure(f"File '{chn_file_name}' not exists in JSON-file!")


def test_rtn_file_mode():
    global new_file_name1, new_file_name2, del_file_name, chn_file_name
    # test NEW file mode
    print("\nNow... return all files back...")
    # restore new_file_name1
    rtn_cmd = f"mv -v {new_file_name1} {del_file_name}"
    print(rtn_cmd)
    new_out = os.popen(rtn_cmd)
    for new_str in new_out:
        print(">> " + new_str.strip())
    # list directory
    ls_mission_dir()
    # restore new_file_name2
    rtn_cmd = f"mv -v {new_file_name2} {chn_file_name}"
    print(rtn_cmd)
    new_out = os.popen(rtn_cmd)
    for new_str in new_out:
        print(">> " + new_str.strip())
    # list directory
    ls_mission_dir()


if is_ficheda_running():
    print("Daemon 'ficheda' already running!")
    op_cmd = "Y"
    # cmd = input("Finish him? [y/N]: ")
    if op_cmd.lower() == "y":
        print("With pleasure ;)...")
        print("killall -TERM ficheda")
        os.popen("killall -TERM ficheda")
        time.sleep(wait_a_few_seconds)
        if is_ficheda_running():
            print("Hmmm... a hard nut to crack :)")
            print("killall -KILL ficheda")
            os.popen("killall -KILL ficheda")
            time.sleep(wait_a_few_seconds)
            if is_ficheda_running():
                print("Mission impossible :(")
                exit(0)
    else:
        print("As you wish :)")
        print("Bye!")
        exit(0)

print("Remove mission directory and json file...")
op_cmd = "rm -vr /tmp/ficheda/"
print(op_cmd)
op_out = os.popen(op_cmd)
for op_str in op_out:
    print(">> " + op_str.strip())
op_cmd = "rm -v /tmp/ficheda.json"
print(op_cmd)
op_out = os.popen(op_cmd)
for op_str in op_out:
    print(">> " + op_str.strip())

print("\nNow... create mission directory...")
print("mkdir " + mission_dir)
try:
    os.mkdir(mission_dir)
except OSError as error:
    print("\nSorry...")
    print(error)
    exit(0)

files_dict = {}
print("Create some files:")
for i in range(num_ori_files):
    file_name = mission_dir + "file_" + str(i).zfill(4) + ".data"
    create_fill_new_file(file_name)
    files_dict[file_name] = "0x" + crc32(file_name)
    print(" " + file_name + ": " + files_dict[file_name])
# print(files_dict)
print("\nSuccess!")

print("\nNow... start 'ficheda' daemon!")
print(mission_cmd)
os.popen(mission_cmd)
time.sleep(wait_a_few_seconds)
if not is_ficheda_running():
    print("\nDaemon not started... Check syslog messages.")
    exit(0)
print("Daemon started.")

print("\nWait a few seconds...")
time.sleep(wait_a_few_seconds)
ficheda_sigusr1()

new_file_name1 = ""
new_file_name2 = ""
del_file_name = ""
chn_file_name = ""

the_first_file_must_be_perfect()
test_new_file_mode()
test_del_file_mode()
test_chn_file_mode()
test_rtn_file_mode()
the_last_file_must_be_perfect()

# finish
print("\nWait a few seconds...")
time.sleep(wait_a_few_seconds)
ficheda_sigterm()
