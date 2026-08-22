from flask import Flask, render_template, request, redirect, url_for, session
import os
from datetime import datetime, timedelta
import serial
import json
import threading
import time
import re

app = Flask(__name__)
app.secret_key = 'lockednet_super_secret_key'

serial_port = None
serial_lock = threading.Lock()

def init_serial():
    global serial_port
    try:
        serial_port = serial.Serial('COM3', 115200, timeout=1)
        serial_port.setDTR(False)
        serial_port.setRTS(False)
        print("[ПК] Серійний порт COM3 відкрито!")
        
        time.sleep(2)
        with serial_lock:
            serial_port.write(b"1|SYNC|wifi\n")
    except Exception as e:
        print("[ПК] Помилка відкриття порту:", e)

def write_log(user_identifier, command_text):
    now_dt = datetime.now()
    now = now_dt.strftime("%d/%b/%Y %H:%M:%S").upper()
    log_entry = f'"{now}" - - {user_identifier} give the command "{command_text}"\n'
    
    date_str = now_dt.strftime("%Y-%m-%d")
    log_filename = f"{date_str}.txt"
    log_path = os.path.join(os.path.dirname(__file__), '..', 'LOG', log_filename)
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    
    with open(log_path, 'a', encoding='utf-8') as f:
        f.write(log_entry)

def process_log_request():
    log_lines = []
    today = datetime.now()
    yesterday = today - timedelta(days=1)
    log_files = [f"{yesterday.strftime('%Y-%m-%d')}.txt", f"{today.strftime('%Y-%m-%d')}.txt"]
    
    for filename in log_files:
        log_path = os.path.join(os.path.dirname(__file__), '..', 'LOG', filename)
        if os.path.exists(log_path):
            with open(log_path, 'r', encoding='utf-8') as f:
                log_lines.extend(f.readlines())
    
    for line in log_lines[-5:]:
        m = re.search(r'"\d+/\w+/\d+ (\d{2}:\d{2}):\d{2}" - - (\S+) give the command "(.*?)"', line)
        if m:
            time_str = m.group(1)      
            user = m.group(2)[:4]
            action = m.group(3)[:4]
            short_log = f"{time_str} {user}:{action}"
            
            payload = f"1|LOG:{short_log}|beacon\n".encode('utf-8')
            with serial_lock:
                if serial_port and serial_port.is_open:
                    serial_port.write(payload)
            time.sleep(5.5) 

def serial_listener():
    global serial_port
    while True:
        if serial_port and serial_port.is_open:
            try:
                if serial_port.in_waiting > 0:
                    line = serial_port.readline().decode('utf-8', errors='ignore').strip()
                    if line:
                        if line.startswith("SYNC_LOG|"):
                            parts = line.split('|', 2)
                            if len(parts) == 3:
                                filename = parts[1]
                                content = parts[2]
                                formatted_date = f"{filename[:4]}-{filename[4:6]}-{filename[6:8]}.txt"
                                log_path = os.path.join(os.path.dirname(__file__), '..', 'LOG', formatted_date)
                                
                                os.makedirs(os.path.dirname(log_path), exist_ok=True)
                                with open(log_path, 'a', encoding='utf-8') as f:
                                    f.write(content + '\n')
                        elif line == "1|log|beacon":
                            print("[ПК] Отримано запит логів від Брєлка!")
                            process_log_request()
                        elif "|beacon" in line and not line.startswith("1|LOG:"):
                            parts = line.split('|')
                            if len(parts) == 3:
                                mod_id, cmd, src = parts
                                write_log("Beacon", cmd)
            except Exception:
                pass
        time.sleep(0.1) 

def ping_hub():
    global serial_port
    while True:
        time.sleep(10)
        with serial_lock:
            if serial_port and serial_port.is_open:
                try:
                    serial_port.write(b"1|PING|wifi\n")
                except Exception:
                    pass

init_serial()
threading.Thread(target=serial_listener, daemon=True).start()
threading.Thread(target=ping_hub, daemon=True).start()

# web routes
@app.route('/', methods=['GET', 'POST'])
def login():
    error = None
    if request.method == 'POST':
        username = request.form.get('username')
        password = request.form.get('password')

        users_file_path = os.path.join(os.path.dirname(__file__), 'users.json')
        users = {}
        if os.path.exists(users_file_path):
            with open(users_file_path, 'r', encoding='utf-8') as f:
                users = json.load(f)

        if username in users and users[username] == password:
            session['username'] = username
            return redirect(url_for('menu'))
        else:
            error = "Go away"

    return render_template('login.html', error=error)

@app.route('/menu')
def menu():
    return render_template('menu.html')

@app.route('/logout')
def logout():
    session.pop('username', None)
    return redirect(url_for('login'))

@app.route('/maintaince')
def maintaince():
    return render_template('maintaince.html')

@app.route('/devices', methods=['GET', 'POST'])
def devices():
    message = None
    serial_error = None
    if request.method == 'POST':
        action = request.form.get('device_action')
        if action:
            module_id, cmd, src = action.split('|')
            
            payload = f"{module_id}|{cmd}|{src}\n".encode('utf-8')
            with serial_lock:
                if serial_port and serial_port.is_open:
                    serial_port.write(payload)
                else:
                    serial_error = "Помилка: Хаб не підключено"
            
            command_text = ""
            if module_id == "bD" and cmd == "Boil": 
                message, command_text = 'Команда відправлена', "Boil the water"
            elif module_id == "mD" and cmd == "Measure": 
                message, command_text = 'Команда відправлена', "Measure the air"
            elif module_id == "Ud" and cmd == "Voltage": 
                message, command_text = 'Команда відправлена', "Check voltage"
            elif module_id == "cA" and cmd == "Fresh": 
                message, command_text = 'Команда відправлена', "Fresh air"
            elif module_id == "cA" and cmd == "Warm":
                message, command_text = 'Команда відправлена', "Warm the room"
                
            user_identifier = session.get('username', request.remote_addr)
            write_log(user_identifier, command_text)
                
    return render_template('devices.html', message=message, serial_error=serial_error)

@app.route('/hub', methods=['GET', 'POST'])
def hub():
    message = None
    serial_error = None
    if request.method == 'POST':
        action = request.form.get('hub_action')
        if action:
            module_id, cmd, src = action.split('|')
            payload = f"{module_id}|{cmd}|{src}\n".encode('utf-8')
            with serial_lock:
                if serial_port and serial_port.is_open:
                    serial_port.write(payload)
                else:
                    serial_error = "Помилка: Хаб не підключено"
            
            command_text = ""
            if cmd == "Buzz":
                message, command_text = 'Команда відправлена', "Buzz"
            elif cmd == "Charge":
                message, command_text = 'Запит відправлено', "Hub charge"
            elif cmd == "Time":
                message, command_text = 'Запит відправлено', "Check the time"
            elif cmd == "OLED":
                message, command_text = 'Команда відправлена', "Check OLED"
            elif cmd == "Connecting":
                message, command_text = 'Запит відправлено', "Check PC connection"
                
            user_identifier = session.get('username', request.remote_addr)
            write_log(user_identifier, command_text)
                
    return render_template('hub.html', message=message, serial_error=serial_error)

@app.route('/abprotocol', methods=['GET', 'POST'])
def abprotocol():
    status = session.get('ab_status', 'OFF')
    settings = session.get('ab_settings', {
        'ups_interval': 5, 'meteo_interval': 10,
        'min_temp': 15.0, 'max_hum': 60, 'max_co2': 1000
    })
    
    message = None
    serial_error = None

    if request.method == 'POST':
        payload = None
        command_text = ""

        if 'ab_action' in request.form:
            payload = request.form['ab_action']
            cmd = payload.split('|')[1]
            
            if cmd == "alarm":
                status = "ON"
                message = "Протокол діє"
            else:
                status = "OFF"
                message = "Протокол вимкнено"
                
            session['ab_status'] = status
            command_text = f"ABP {status}"
        elif 'set_ups_interval' in request.form:
            interval = request.form['ups_interval']
            settings['ups_interval'] = interval 
            payload = f"1|abp_set_ups:{interval}|wifi"
            command_text = f"Set UPS interval to {interval} min"
        elif 'set_meteo_interval' in request.form:
            interval = request.form['meteo_interval']
            settings['meteo_interval'] = interval
            payload = f"1|abp_set_meteo:{interval}|wifi"
            command_text = f"Set Meteo interval to {interval} min"
        elif 'set_min_temp' in request.form:
            temp = request.form['min_temp']
            settings['min_temp'] = temp
            payload = f"1|abp_set_temp:{temp}|wifi"
            command_text = f"Set min temperature to {temp} C"
        elif 'set_max_hum' in request.form:
            hum = request.form['max_hum']
            settings['max_hum'] = hum
            payload = f"1|abp_set_hum:{hum}|wifi"
            command_text = f"Set max humidity to {hum} %"
        elif 'set_max_co2' in request.form:
            co2 = request.form['max_co2']
            settings['max_co2'] = co2
            payload = f"1|abp_set_co2:{co2}|wifi"
            command_text = f"Set max CO2 to {co2} ppm"

        session['ab_settings'] = settings 

        if payload:
            message = "Налаштування відправлено"
            write_log(session.get('username', 'local'), command_text)
            with serial_lock:
                if serial_port and serial_port.is_open:
                    serial_port.write(payload.encode('utf-8') + b'\n')
                else:
                    serial_error = "Помилка: Хаб не підключено"

    return render_template('abprotocol.html', status=status, settings=settings, message=message, serial_error=serial_error)

@app.route('/log')
def log():
    log_lines = []
    
    today = datetime.now()
    yesterday = today - timedelta(days=1)
    
    log_files = [f"{yesterday.strftime('%Y-%m-%d')}.txt", f"{today.strftime('%Y-%m-%d')}.txt"]
    
    for filename in log_files:
        log_path = os.path.join(os.path.dirname(__file__), '..', 'LOG', filename)
        if os.path.exists(log_path):
            with open(log_path, 'r', encoding='utf-8') as f:
                log_lines.extend(f.readlines())
            
    log_lines.reverse()
    return render_template('log.html', logs=log_lines)

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=False) 
