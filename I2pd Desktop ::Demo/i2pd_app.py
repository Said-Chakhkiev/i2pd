import tkinter as tk
from tkinter import messagebox, scrolledtext
import subprocess
import platform
import webbrowser
import os

class I2PDApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("I2pd Desktop")
        self.geometry("600x400")
        self.create_widgets()
        self.check_i2pd_installed()
        self.update_status()

    def create_widgets(self):
        self.status_label = tk.Label(self, text="Status: Unknown")
        self.status_label.pack(pady=5)

        self.install_button = tk.Button(self, text="Install I2pd", command=self.install_i2pd)
        self.install_button.pack(pady=5)

        self.start_button = tk.Button(self, text="Start I2pd", command=self.start_i2pd)
        self.start_button.pack(pady=5)

        self.stop_button = tk.Button(self, text="Stop I2pd", command=self.stop_i2pd)
        self.stop_button.pack(pady=5)

        self.open_console_button = tk.Button(self, text="Open I2pd Console", command=self.open_console)
        self.open_console_button.pack(pady=5)

        self.config_button = tk.Button(self, text="Edit Configuration", command=self.edit_config)
        self.config_button.pack(pady=5)

        self.log_text = scrolledtext.ScrolledText(self, wrap=tk.WORD, width=50, height=15)
        self.log_text.pack(pady=5)

        self.update_log_button = tk.Button(self, text="Update Log", command=self.update_log)
        self.update_log_button.pack(pady=5)

        self.update_status_button = tk.Button(self, text="Update Status", command=self.update_status)
        self.update_status_button.pack(pady=5)

    def log_message(self, message):
        self.log_text.insert(tk.END, message + "\n")
        self.log_text.see(tk.END)

    def check_i2pd_installed(self):
        try:
            if platform.system() == "Darwin":
                subprocess.run(["brew", "list", "i2pd"], capture_output=True, text=True, check=True)
            elif platform.system() == "Linux":
                subprocess.run(["dpkg", "-s", "i2pd"], capture_output=True, text=True, check=True)
            elif platform.system() == "Windows":
                subprocess.run(["choco", "list", "--local-only", "i2pd"], capture_output=True, text=True, check=True)
            self.log_message("I2pd is already installed.")
        except subprocess.CalledProcessError:
            self.log_message("I2pd is not installed. Please install it first.")
            self.status_label.config(text="Status: Not Installed")

    def install_i2pd(self):
        try:
            if platform.system() == "Darwin":
                subprocess.run(["brew", "install", "i2pd"], capture_output=True, text=True, check=True)
            elif platform.system() == "Linux":
                subprocess.run(["sudo", "apt", "update"], capture_output=True, text=True, check=True)
                subprocess.run(["sudo", "apt", "install", "-y", "i2pd"], capture_output=True, text=True, check=True)
            elif platform.system() == "Windows":
                subprocess.run(["choco", "install", "i2pd", "-y"], capture_output=True, text=True, check=True)
            self.log_message("I2pd installed successfully.")
            self.status_label.config(text="Status: Installed")
        except subprocess.CalledProcessError as e:
            self.log_message(f"Failed to install i2pd: {e.output}")
            messagebox.showerror("Error", f"Failed to install i2pd: {e}")

    def start_i2pd(self):
        try:
            if platform.system() == "Darwin":
                result = subprocess.run(["brew", "services", "list"], capture_output=True, text=True)
                if "i2pd" in result.stdout and "started" in result.stdout:
                    self.log_message("I2pd is already started.")
                else:
                    result = subprocess.run(["brew", "services", "start", "i2pd"], capture_output=True, text=True, check=True)
                    self.log_message(result.stdout)
            elif platform.system() == "Linux":
                result = subprocess.run(["sudo", "systemctl", "start", "i2pd"], capture_output=True, text=True, check=True)
                self.log_message(result.stdout)
            elif platform.system() == "Windows":
                result = subprocess.run(["sc", "start", "i2pd"], capture_output=True, text=True, check=True)
                self.log_message(result.stdout)
            self.update_status()
            self.log_message("I2pd started successfully.")
        except subprocess.CalledProcessError as e:
            self.log_message(f"Failed to start i2pd: {e.output}")
            messagebox.showerror("Error", f"Failed to start i2pd: {e}")

    def stop_i2pd(self):
        try:
            if platform.system() == "Darwin":
                result = subprocess.run(["brew", "services", "list"], capture_output=True, text=True)
                if "i2pd" in result.stdout and "stopped" in result.stdout:
                    self.log_message("I2pd is already stopped.")
                else:
                    result = subprocess.run(["brew", "services", "stop", "i2pd"], capture_output=True, text=True, check=True)
                    self.log_message(result.stdout)
            elif platform.system() == "Linux":
                result = subprocess.run(["sudo", "systemctl", "stop", "i2pd"], capture_output=True, text=True, check=True)
                self.log_message(result.stdout)
            elif platform.system() == "Windows":
                result = subprocess.run(["sc", "stop", "i2pd"], capture_output=True, text=True, check=True)
                self.log_message(result.stdout)
            self.update_status()
            self.log_message("I2pd stopped successfully.")
        except subprocess.CalledProcessError as e:
            self.log_message(f"Failed to stop i2pd: {e.output}")
            messagebox.showerror("Error", f"Failed to stop i2pd: {e}")

    def open_console(self):
        webbrowser.open("http://127.0.0.1:7070/")

    def edit_config(self):
        config_path = ""
        if platform.system() == "Darwin":
            config_path = os.path.expanduser("~/Library/Application Support/i2pd/i2pd.conf")
        elif platform.system() == "Linux":
            config_path = "/etc/i2pd/i2pd.conf"
        elif platform.system() == "Windows":
            config_path = os.path.expanduser("~/AppData/Roaming/i2pd/i2pd.conf")
        
        if config_path and os.path.exists(config_path):
            subprocess.run(["open" if platform.system() == "Darwin" else "xdg-open" if platform.system() == "Linux" else "notepad", config_path])
        else:
            messagebox.showerror("Error", f"Configuration file not found: {config_path}")

    def update_log(self):
        log_path = ""
        if platform.system() == "Darwin":
            log_path = os.path.expanduser("~/Library/Application Support/i2pd/i2pd.log")
        elif platform.system() == "Linux":
            log_path = "/var/log/i2pd.log"
        elif platform.system() == "Windows":
            log_path = os.path.expanduser("~/AppData/Roaming/i2pd/i2pd.log")
        
        if log_path and os.path.exists(log_path):
            with open(log_path, "r") as log_file:
                self.log_text.delete(1.0, tk.END)
                self.log_text.insert(tk.END, log_file.read())
        else:
            messagebox.showerror("Error", f"Log file not found: {log_path}")

    def update_status(self):
        try:
            if platform.system() == "Darwin":
                result = subprocess.run(["brew", "services", "list"], capture_output=True, text=True, check=True)
                status = "stopped"
                for line in result.stdout.splitlines():
                    if "i2pd" in line:
                        if "started" in line:
                            status = "started"
                        break
            elif platform.system() == "Linux":
                result = subprocess.run(["sudo", "systemctl", "is-active", "i2pd"], capture_output=True, text=True, check=True)
                status = result.stdout.strip()
            elif platform.system() == "Windows":
                result = subprocess.run(["sc", "query", "i2pd"], capture_output=True, text=True, check=True)
                status = "stopped"
                for line in result.stdout.splitlines():
                    if "RUNNING" in line:
                        status = "started"
                        break
            
            self.status_label.config(text=f"Status: {status}")
            self.log_message(f"I2pd status: {status}")
        except subprocess.CalledProcessError as e:
            self.log_message(f"Failed to get i2pd status: {e.output}")
            messagebox.showerror("Error", f"Failed to get i2pd status: {e}")
            
if __name__ == "__main__":
    app = I2PDApp()
    app.mainloop()
