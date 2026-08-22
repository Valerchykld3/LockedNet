setInterval(() => {
    fetch('/api/feedback')
        .then(response => response.text())
        .then(text => {
            if(text.trim() !== "") {
                let fbElement = document.getElementById('feedbackText');
                if(fbElement) {
                    fbElement.innerText = "Фідбек: " + text;
                } else {
                    alert("Повідомлення від Hub: " + text);
                }
            }
        })
        .catch(err => console.log(err));

    let logBox = document.getElementById('logBox');
    if(logBox) {
        fetch('/api/logs')
            .then(response => response.text())
            .then(text => {
                if(text.trim() !== "") {
                    if (logBox.innerHTML.includes("Press \"Request Logs\"")) {
                        logBox.innerHTML = ""; 
                    }
                    logBox.innerHTML += text;
                    logBox.scrollTop = logBox.scrollHeight;
                }
            })
            .catch(err => console.log(err));
    }
}, 1500);