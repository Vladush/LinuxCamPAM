# LinuxCamPAM Visual Guides

This document contains visual guides to help you configure, troubleshoot, and use LinuxCamPAM effectively.

## 1. Troubleshooting Wizard: "Why is it failing?"

Use this flowchart to diagnose authentication issues.

```mermaid
graph TD
    Start[Authentication Failed] --> Q1{Did the Camera/IR Light Turn On?}
    
    Q1 -- No --> CheckService[Check Service Status]
    CheckService --> Cmd1["sudo systemctl status linuxcampam"]
    Cmd1 --> Q2{Is Active/Running?}
    Q2 -- No --> Restart[sudo systemctl restart linuxcampam]
    Q2 -- Yes --> CheckLog["journalctl -u linuxcampam -f"]
    CheckLog --> Err{See Error?}
    Err -- "Device Busy" --> CloseApp[Close other camera apps &#40;Cheese, Zoom&#41;]
    Err -- "No Camera" --> Config[Check /etc/linuxcampam/config.ini path]

    Q1 -- Yes --> Q3{Did it recognize you?}
    Q3 -- No/Unknown --> TestCmd["Run: linuxcampam test"]
    TestCmd --> Result{Result?}
    
    Result -- "No Face Detected" --> Lighting[Check Lighting / Position]
    Lighting --> EnrollAgain
    
    Result -- "Face Detected, Low Score" --> Retrain[Retrain Model]
    Retrain --> CmdTrain["linuxcampam train <user>"]
    
    Result -- "Too Dark" --> MinBright[Lower 'min_brightness' in config]
    
    Result -- "Success" --> PAM{Fails only in Sudo?}
    PAM -- Yes --> PAMConfig[Check /etc/pam.d/sudo]
    
    style Start fill:#f96,stroke:#333
    style Cmd1 fill:#eee,stroke:#333,stroke-dasharray: 5 5
    style CmdTrain fill:#eee,stroke:#333,stroke-dasharray: 5 5
    style TestCmd fill:#eee,stroke:#333,stroke-dasharray: 5 5
```

## 2. Configuration Helper: "Which Setup is Right for Me?"

Choose the right configuration based on your hardware.

```mermaid
graph TD
    Start[I want to set up LinuxCamPAM] --> Q1{Do you have an IR Emitter?}
    
    Q1 -- Yes --> Q2{Do you have a separate RGB Webcam?}
    Q2 -- Yes --> Dual[**Dual Camera Setup**]
    Dual --> C1[Use Policy: **adaptive**]
    C1 --> C2[Set IR Camera as **Mandatory**]
    C2 --> C3[Set RGB Camera as **Conditional**]
    
    Q2 -- No --> SingleIR[**Single IR Setup**]
    SingleIR --> C4[Use Policy: **strict**]
    
    Q1 -- No --> RGB[**Standard RGB Webcam**]
    RGB --> Warn[**Security Warning**: RGB is easier to spoof]
    Warn --> C5[Use Policy: **lenient**]
    C5 --> C6[Set min_brightness: 40+]
```

## 3. The Authentication Process (Simplified)

How the system decides to let you in.

```mermaid
sequenceDiagram
    participant You
    participant Cam as Camera
    participant Brain as AI Model
    participant Door as Access (sudo/login)

    You->>Door: Request Access
    Door->>Brain: "Is this <User>?"
    Brain->>Cam: "Look for user..."
    
    activate Cam
    Cam-->>Brain: [Image Frame]
    deactivate Cam
    
    Brain->>Brain: 1. Is it bright enough?
    Brain->>Brain: 2. Is there a face?
    Brain->>Brain: 3. Does it look like <User>?
    
    alt Match > Threshold
        Brain-->>Door: YES (Unlock)
        Door-->>You: Success!
    else No Match / Timeout
        Brain-->>Door: NO
        Door-->>You: Password Required
    end
```

## 4. Enrollment Quality Loop

Bad enrollment data is the #1 cause of failure. Follow this loop for best results.

```mermaid
stateDiagram-v2
    [*] --> GoodLighting
    GoodLighting --> Capture: Run 'linuxcampam add user'
    
    state "Capture Conditions" as Capture {
        AvoidBacklight: No window behind you
        StraightFace: Look directly at camera
        NoGlasses: Remove heavy reflection
    }
    
    Capture --> Verify: Run 'linuxcampam test'
    
    state "Verification" as Verify {
        CheckScore: Score > 0.6 is GOOD
        CheckSpeed: Time < 300ms is GOOD
    }
    
    Verify --> Quality{Is Quality Good?}
    Quality --> Yes: Done
    Quality --> No: TrainMore
    
    TrainMore: Run 'linuxcampam train user'
    TrainMore --> Capture: (Add variations: glasses, angles)
```
