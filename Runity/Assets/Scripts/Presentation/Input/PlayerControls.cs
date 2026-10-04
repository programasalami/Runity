using System;
using UnityEngine;
using UnityEngine.InputSystem;

namespace Runity.Presentation.Input
{
    /// <summary>Game input through the Input System, defined in code so rebinding UI can be added later without touching gameplay.
    /// Reference bindings (CLAUDE.md / Settings): WASD move, Q/E turn the camera, Z resets it, Enter opens chat.
    /// Left mouse fires at the pointer, F enters a portal, R returns to the Nexus.</summary>
    public sealed class PlayerControls : IDisposable
    {
        private readonly InputAction _move;
        private readonly InputAction _turn;
        private readonly InputAction _resetCamera;
        private readonly InputAction _chat;
        private readonly InputAction _fire;
        private readonly InputAction _aim;
        private readonly InputAction _interact;
        private readonly InputAction _nexus;

        public PlayerControls()
        {
            _move = new InputAction("Move", InputActionType.Value);
            _move.AddCompositeBinding("2DVector").With("Up", "<Keyboard>/w").With("Down", "<Keyboard>/s").With("Left", "<Keyboard>/a").With("Right", "<Keyboard>/d");
            _move.AddCompositeBinding("2DVector").With("Up", "<Keyboard>/upArrow").With("Down", "<Keyboard>/downArrow").With("Left", "<Keyboard>/leftArrow").With("Right", "<Keyboard>/rightArrow");
            _move.AddBinding("<Gamepad>/leftStick");

            _turn = new InputAction("Turn", InputActionType.Value);
            _turn.AddCompositeBinding("1DAxis").With("Negative", "<Keyboard>/q").With("Positive", "<Keyboard>/e");
            _turn.AddCompositeBinding("1DAxis").With("Negative", "<Gamepad>/leftShoulder").With("Positive", "<Gamepad>/rightShoulder");

            _resetCamera = new InputAction("ResetCamera", InputActionType.Button, "<Keyboard>/z");
            _chat = new InputAction("Chat", InputActionType.Button, "<Keyboard>/enter");
            _chat.AddBinding("<Keyboard>/numpadEnter");

            _fire = new InputAction("Fire", InputActionType.Button, "<Mouse>/leftButton");
            _fire.AddBinding("<Gamepad>/rightTrigger");
            _aim = new InputAction("Aim", InputActionType.Value, "<Pointer>/position");
            _interact = new InputAction("Interact", InputActionType.Button, "<Keyboard>/f");
            _interact.AddBinding("<Gamepad>/buttonSouth");
            _nexus = new InputAction("Nexus", InputActionType.Button, "<Keyboard>/r");

            _fire.Enable();
            _aim.Enable();
            _interact.Enable();
            _nexus.Enable();
            _move.Enable();
            _turn.Enable();
            _resetCamera.Enable();
            _chat.Enable();
        }

        public Vector2 Move => _move.ReadValue<Vector2>();
        public float Turn => _turn.ReadValue<float>();
        public bool ResetCameraPressed => _resetCamera.WasPressedThisFrame();
        public bool ChatPressed => _chat.WasPressedThisFrame();
        public bool FireHeld => _fire.IsPressed();
        public Vector2 AimScreenPosition => _aim.ReadValue<Vector2>();
        public bool InteractPressed => _interact.WasPressedThisFrame();
        public bool NexusPressed => _nexus.WasPressedThisFrame();

        public void Dispose()
        {
            _move.Dispose();
            _turn.Dispose();
            _resetCamera.Dispose();
            _chat.Dispose();
            _fire.Dispose();
            _aim.Dispose();
            _interact.Dispose();
            _nexus.Dispose();
        }
    }
}
